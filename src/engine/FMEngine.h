#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "FMVoice.h"
#include <vector>

class FMEngine : public EngineBase
{
public:
    enum Param
    {
        Op1Ratio = 0, Op1Level, Op1Detune, Op1Feedback,
        Op1Attack, Op1Decay, Op1Sustain, Op1Release,
        Op2Ratio = 10, Op2Level, Op2Detune, Op2Feedback,
        Op2Attack, Op2Decay, Op2Sustain, Op2Release,
        Op3Ratio = 20, Op3Level, Op3Detune, Op3Feedback,
        Op3Attack, Op3Decay, Op3Sustain, Op3Release,
        Op4Ratio = 30, Op4Level, Op4Detune, Op4Feedback,
        Op4Attack, Op4Decay, Op4Sustain, Op4Release,
        Op5Ratio = 40, Op5Level, Op5Detune, Op5Feedback,
        Op5Attack, Op5Decay, Op5Sustain, Op5Release,
        Op6Ratio = 50, Op6Level, Op6Detune, Op6Feedback,
        Op6Attack, Op6Decay, Op6Sustain, Op6Release,
        AlgorithmParam = 60,
        MasterLevel = 61,
        TuneParam = 62,         // semitones
        BendRangeParam = 63,    // semitones
        // 64 and 65 were XIN and XDPT (the track input on an operator's phase). They are slots of
        // the modulation router now: the XIN page's DST1 = PM1 .. PM6 / PMAL and AMT1 = the depth.
        // A value stored under the old ids is still taken, as those two cells (setParameter).
        LegacyXinParam = 64,
        LegacyXinDepthParam = 65,
        // What the operators read (dsp/PhaseOsc.h). Each operator's WAVE is id op * 10 + 8 (an
        // index into phaseosc::waveLabels(): SINE, CYCL, then the built-in tables; stored, the
        // list grows at the end) and its position op * 10 + 9 (0..1).
        CycleSample = 66,       // SMPL: the sample of the bank whose cycles the wave CYCL reads
        SeqSteps = 67,          // WSEQ: 0 = the position scans a table; 1-15 = a sequence of 2-16 of its waves
        SeqFade = 68,           // XFAD: the share of a sequence step spent crossfading into the next wave
        PositionEnv = 69,       // PENV: each operator's own envelope moves its position, -1..1
        VoiceCountParam = 70,   // VOX: polyphony 1..16 (default 8)
        FilterBase = 100        // 100-115: the per-voice filter (VoiceFilterSection offsets)
        // 158-189 and 700-772 are the modulation router's (mod::Router: LFO1, LFO2, STRIP, XIN,
        // the slots, the custom LFO shape): everything that moves this engine's sound goes
        // through it, the pad's pressure routes and the track input at audio rate included.
    };

    static constexpr int MAX_VOICES = 16;

    FMEngine(int maxVoices = 8)
        : allocator(MAX_VOICES)
    {
        voices.resize((size_t) MAX_VOICES);
        for (auto& voice : voices)
            voice.setLevel(masterLevel);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t) voiceIdx].noteOn(router, v.note, v.velocity);
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // Init patch: a two-operator sound (2 → 1) with the other operators silent, so adding an
        // operator is a matter of raising its level. Every page parameter is seeded so that
        // getParameter is always the true value (and later sets never allocate).
        // The per-note randoms (RAND, RND2 ..) repeat from run to run: the same notes in the same
        // order on a new engine draw the same values. (This engine has no ANLG page, so no SEED
        // cell and no spread generator to hand over: the seed is fixed, as the macro oscillator's.)
        for (int v = 0; v < MAX_VOICES; ++v)
            voices[(size_t) v].getMod().seed(0, v);

        for (auto& page : getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.id == VoiceCountParam ? (float) maxVoices : p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
    }

    static constexpr int OP_WAVE = 8, OP_POSITION = 9;      // an operator's cells beside its first eight
    static constexpr int MAX_SEQ_STEPS = 16;

    static const ParamPages& pages()
    {
        // (The engine's own pages and the filter's: what the router's table is built from.)
        static const char* const algos[] = { "6>1", "2+3", "3x2", "2x3", "1>3", "ADD", "2+4", "4+2" };
        static const ParamPages list = build(algos);
        return list;
    }

    // What can be moved (the modulation router's table). The order is the DEST list as it was
    // before the router, so a stored DEST keeps its meaning: the original eight (pitch, level, pan,
    // cutoff, resonance, index, feedback and detune as inputs of the voice), then every continuous
    // cell of the pages - the operator cells carry the operator's number (LVL1, RAT1, DET1, FDB1,
    // ATK1 ...) -, the filter's, LRAT / LDPT and the operators' positions (POS1 ..). After them,
    // new with the router: ETIM (every operator's envelope time) and FTIM (the filter's), and the
    // operators' phases PM1 .. PM6 and PMAL (all six): radians added to an operator's phase, 1 at
    // full depth of a source at full swing - the index of a modulator at full level.
    //   XIN > PMn   the track input as a seventh modulator (what the XIN / XDPT cells were:
    //               the XIN page's DST and AMT)
    //   OPm > PMn   a connection no algorithm has, heard one sub-block late (the algorithms'
    //               own connections stay within the sample)
    // Per sample (so an audio-rate source reaches them): PTCH, AMP, PAN, CUT, INDX, FDBK, the
    // positions and the phases. Not offered: ALGO, WAVn, SMPL, WSEQ, VOX (choices and counts),
    // TUNE and BEND (whole semitones; PTCH is the pitch), LEVL (the level of the mixed voices;
    // AMP is each voice's), and what the filter leaves out.
    // (A free slot's own destinations, L1RT .., start where the visible list ends: they are nine
    // places further on than they were.)
    static const mod::DestTable& lfoDests()
    {
        static const mod::DestTable table = []
        {
            static const char* const names[6][6] = {
                { "DET1", "DET2", "DET3", "DET4", "DET5", "DET6" }, { "FDB1", "FDB2", "FDB3", "FDB4", "FDB5", "FDB6" },
                { "ATK1", "ATK2", "ATK3", "ATK4", "ATK5", "ATK6" }, { "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6" },
                { "SUS1", "SUS2", "SUS3", "SUS4", "SUS5", "SUS6" }, { "REL1", "REL2", "REL3", "REL4", "REL5", "REL6" } };
            static const char* const phases[6] = { "PM1", "PM2", "PM3", "PM4", "PM5", "PM6" };
            const ParamPages& p = pages();
            mod::DestTable t;
            t.group();
            t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);      // a ratio: one octave x the amount squared
            t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);  // (below 0: ring modulation)
            t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);              // octaves
            t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            t.input("INDX", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample);                // a factor on every modulator's effect
            t.input("FDBK", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);               // an offset of every operator's feedback
            t.input("DETN", 0.0f, FMVoice::LFO_DETUNE_CENTS, -4.0f * FMVoice::LFO_DETUNE_CENTS, 4.0f * FMVoice::LFO_DETUNE_CENTS);
            t.group();
            for (int op = 0; op < 6; ++op) t.add(p, op * 10 + 1);       // LVL1..6 (FMMIX)
            t.group();
            for (int op = 0; op < 6; ++op) t.add(p, op * 10 + 0);       // RAT1..6 (RATIO)
            for (int op = 0; op < 6; ++op)
            {
                t.group();
                for (int cell = 2; cell <= 7; ++cell)
                    t.add(p, op * 10 + cell, names[cell - 2][op]);
            }
            VoiceFilterSection::addLfoDests(t, p, FilterBase);
            t.addCross();
            t.group();
            for (int op = 0; op < 6; ++op) t.add(p, op * 10 + OP_POSITION, nullptr, mod::PerSample);    // POS1..6 (WPOS)
            jassert(t.indexOfLabel("POS1") == FMVoice::LfoPosition);
            // New with the router (after what was there before: a stored DEST keeps its meaning).
            t.group();
            t.envTimes();
            jassert(t.ampTime() == FMVoice::LfoEnvTime && t.filterTime() == FMVoice::LfoFilterTime);
            t.group();
            // (Phases in radians: a whole period is 2 pi. The router lets them go and brings them in
            // without a step: mod::Phase.)
            for (int op = 0; op < 6; ++op)
                t.input(phases[op], 0.0f, 1.0f, -PHASE_LIMIT, PHASE_LIMIT, mod::PerSample | mod::Phase).period(dsp::TWO_PI_F);
            t.input("PMAL", 0.0f, 1.0f, -PHASE_LIMIT, PHASE_LIMIT, mod::PerSample | mod::Phase).period(dsp::TWO_PI_F);
            jassert(t.indexOfLabel("PM1") == FMVoice::LfoPhase && t.indexOfLabel("PMAL") == FMVoice::LfoPhaseAll);
            jassert(t.size() == FMVoice::LfoCount);
            // The mod strip until the STRIP page says otherwise: the modulation index up to three
            // times (from the strip INDX counts double: +100 = x 3) and a quarter-tone of vibrato.
            t.stripScale(FMVoice::LfoIndex, STRIP_INDEX_SCALE);
            t.stripRoute(0, FMVoice::LfoIndex, 1.0f);
            t.stripVibrato(0.25f);
            // Pad pressure until AT ROUTE says otherwise: the level, a gain of 1 + pressure.
            t.pressureRoute(0, "AMP", 1.0f);
            t.finish(2);
            return t;
        }();
        static const bool registered = (table.registerGroups(), true);
        juce::ignoreUnused(registered);
        return table;
    }

    // What a slot can read here beside the router's own sources: the list SRC and VIA show.
    //   OP1 .. OP6   each operator's output (under its envelope, level and velocity), at audio
    //                rate (FMVoice::PubOp + operator): on PMn it is a connection of one's own
    //                between operators (feedback loops and crossings no algorithm has, one
    //                sub-block late), on CUT filter FM by one operator, on POSn a table scanned
    //                by an operator, on AMP and PAN amplitude modulation and a spread of partials.
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable table = []
        {
            mod::SourceTable t(2);
            for (const char* name : { "OP1", "OP2", "OP3", "OP4", "OP5", "OP6" })
                t.publish(name, mod::AudioRate);
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST.
    // FM DEPTH and TIMBRE are the modulation index, as they always were on this engine.
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return FMVoice::LfoAmp;
            case D::FilterCutoff:   return FMVoice::LfoCutoff;
            case D::FMDepth:        return FMVoice::LfoIndex;
            case D::MacroTimbre:    return FMVoice::LfoIndex;
            case D::PanSpread:      return FMVoice::LfoPan;
            default:                return 0;
        }
    }

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        // SMPL lists the samples the bank holds now (recordings are appended while the app runs).
        auto& bank = SampleBank::get();
        for (auto& page : out)
            for (auto& p : page.params)
                if (p.id == CycleSample)
                {
                    p.choices = bank.names();
                    p.numChoices = bank.size();
                    p.maxVal = (float) std::max(0, bank.size() - 1);
                }
        return out;
    }

    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override { router.setTempo(bpm); }

    void aftertouch(int note, float pressure) override { allocator.setAftertouch(note, pressure); }

    void pitchBend(float amount) override
    {
        bend = juce::jlimit(-1.0f, 1.0f, amount);
        updatePitch();
    }

    // SMPL chooses what a table is cut from: no step lock, no pressure range.
    bool isRebuildParam(ParamId id) const override { return id == CycleSample; }

    EngineType getType() const override { return EngineType::FM; }
    juce::String getName() const override { return "FM Synth"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        router.prepare(sr);
        for (auto& v : voices)
            v.setSampleRate(sr);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        // Once per block: the strip (the cutoff by the filter's WHL amount; its routes are the
        // router's) and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        const float wheelOctaves = filterSection.externalOctaves(wheelNow, 0.0f);
        for (auto& voice : voices)
            voice.getFilter().setExternalOctaves(wheelOctaves);
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, wheelNow, sampleRate);
        const auto setCell = [this](FMVoice& voice, int dest, float value)
        {
            applyVoiceParam(voice, router.dests()[dest].id, value);
        };

        // Sub-blocks: the router's control rate. Inside one the voices are rendered one after
        // the other (they share the router's per-sample buffers).
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            for (int v = 0; v < (int) voices.size(); ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive())
                {
                    // Its LFOs keep time; a cell it was left holding goes back to its set value.
                    if (voice.getMod().idle(router, n))
                        voice.getMod().restore(router, [&](int dest, float value) { voice.setControl(dest, value, setCell); });
                    continue;
                }
                const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
                voice.render(router, mixL.data(), mixR.data(), n, pressure, setCell);
            }
            for (int i = 0; i < n; ++i)
            {
                // LEVL is applied by each voice (so that a note takes it whole)
                const float sampleL = mixL[(size_t) i] * HEADROOM, sampleR = mixR[(size_t) i] * HEADROOM;
                if (right)
                {
                    left[offset + i] += sampleL;
                    right[offset + i] += sampleR;
                }
                else
                {
                    left[offset + i] += 0.5f * (sampleL + sampleR);
                }
            }
            offset += n;
        }
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        soundCut();
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    // ---- Per-stage envelope curves (StageCurveParam): operators 1-6 are envelope slots 0-5 -----
    int getStageCurveParam(ParamId cellId) const override
    {
        const int op = cellId / 10, cell = cellId % 10;
        if (cellId >= 0 && op < 6 && (cell == 4 || cell == 5 || cell == 7))
            return StageCurveParam::id(op, cell == 4 ? StageCurveParam::Attack
                                                     : (cell == 5 ? StageCurveParam::Decay : StageCurveParam::Release));
        return filterSection.stageCurveParamFor(cellId);
    }

private:
    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        const int op = StageCurveParam::envelopeOf(id);
        if (op >= 6)
            return;
        for (auto& voice : voices)
        {
            Envelope::Params p = voice.getOp(op).getEnvelope().getParams();
            stageCurves.applyTo(p, op);
            voice.getOp(op).getEnvelope().setParams(p);
        }
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (id == VoiceCountParam)
        {
            // (The allocator keeps to it: no more than VOX voices are given notes, and a lowered
            // VOX releases the voices past it - they ring out.)
            const int n = juce::jlimit(1, MAX_VOICES, (int) std::lround(value));
            params[id] = (float) n;
            allocator.setVoiceLimit(n);
            return;
        }
        if (filterSection.owns(id))
        {
            if (filterSection.swaps(id, value))
                soundSwapped();
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            router.setBase(id, filterSection.get(id));
            return;
        }
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            return;
        }
        // The XIN and XDPT cells of before the router, from a stored sound: the same thing as
        // the XIN page's first route (the input as it is on PMn / PMAL, and its depth).
        if (id == LegacyXinParam || id == LegacyXinDepthParam)
        {
            // (A file may hold anything in these two: held to the list and to the cell's range.
            // XDPT was 50 until set.)
            if (id == LegacyXinDepthParam)
                legacyXinDepth = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f;
            else
            {
                const int target = std::isfinite(value) ? juce::jlimit(0, FMVoice::NUM_OPS + 1, (int) std::lround(juce::jlimit(-1.0f, 100.0f, value))) : 0;
                router.set(mod::Router::xinId(mod::Router::XinDest1), target == 0 ? 0.0f : (float) (FMVoice::LfoPhase + target - 1));
                router.set(mod::Router::xinId(mod::Router::XinFilter1), 0.0f);
            }
            router.set(mod::Router::xinId(mod::Router::XinAmount1), legacyXinDepth);
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if (id == AlgorithmParam)
            value = (float) juce::jlimit(0, (int) FMVoice::Algorithm::Count - 1, (int) std::lround(value));
        if (id == AlgorithmParam && params[id] != value)
            soundSwapped();
        // What the operators read: choices held to their lists; another wave, or another way of
        // stepping through it, under a sounding note is a swap.
        const bool waveCell = id >= 0 && id < 60 && id % 10 == OP_WAVE;
        if (waveCell || id == SeqSteps || id == CycleSample)
        {
            const int top = waveCell ? phaseosc::waveCount() - 1
                                     : (id == SeqSteps ? MAX_SEQ_STEPS - 1 : std::max(0, SampleBank::get().size() - 1));
            value = std::isfinite(value) ? (float) juce::jlimit(0, top, (int) std::lround(juce::jlimit(-1.0f, 1000.0f, value))) : 0.0f;
            if (params[id] != value && (id != CycleSample || cyclesInUse()))
                soundSwapped();
        }
        if (id == SeqFade || (id >= 0 && id < 60 && id % 10 == OP_POSITION))
            value = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f;
        if (id == PositionEnv)
            value = std::isfinite(value) ? juce::jlimit(-1.0f, 1.0f, value) : 0.0f;
        params[id] = value;
        applyParameter(id, value);
        router.setBase(id, value);
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (mod::Router::owns(id))
            return router.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (mod::Router::owns(id))
            return mod::Router::name(id);
        int opIdx = id / 10;
        int paramIdx = id % 10;
        if (opIdx >= 0 && opIdx < 6)
        {
            juce::String opName = "Op" + juce::String(opIdx + 1) + " ";
            switch (paramIdx)
            {
                case 0: return opName + "Ratio";
                case 1: return opName + "Level";
                case 2: return opName + "Detune";
                case 3: return opName + "Feedback";
                case 4: return opName + "Attack";
                case 5: return opName + "Decay";
                case 6: return opName + "Sustain";
                case 7: return opName + "Release";
                case OP_WAVE: return opName + "Wave";
                case OP_POSITION: return opName + "Position";
                default: break;
            }
        }
        if (id == AlgorithmParam) return "Algorithm";
        if (id == MasterLevel) return "Master Level";
        if (id == TuneParam) return "Tune";
        if (id == BendRangeParam) return "Bend Range";
        if (id == CycleSample) return "Cycle Sample";
        if (id == SeqSteps) return "Wave Sequence";
        if (id == SeqFade) return "Sequence Fade";
        if (id == PositionEnv) return "Position Env";
        if (id == VoiceCountParam) return "Voice Count";
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return 70; }

    void setAlgorithm(FMVoice::Algorithm algo)
    {
        for (auto& v : voices)
            v.setAlgorithm(algo);
    }

    FMVoice& getVoice(int index) { return voices[(size_t) index]; }
    VoiceAllocator& getAllocator() { return allocator; }

private:
    void applyParameter(ParamId id, float value)
    {
        if (id == AlgorithmParam)
        {
            int algoIdx = juce::jlimit(0, (int) FMVoice::Algorithm::Count - 1, (int) value);
            setAlgorithm(static_cast<FMVoice::Algorithm>(algoIdx));
            return;
        }

        if (id == MasterLevel)
        {
            masterLevel = juce::jlimit(0.0f, 1.0f, value);      // (LEVL's own range: a file may hold anything)
            for (auto& voice : voices)
                voice.setLevel(masterLevel);
            return;
        }

        if (id == TuneParam || id == BendRangeParam)
        {
            (id == TuneParam ? tune : bendRange) = value;
            updatePitch();
            return;
        }

        if (id == CycleSample || id == SeqSteps || id == SeqFade)
        {
            // The sample's cycles are made here, when SMPL is set (a rebuild cell: the message
            // thread, never a step lock), whether or not an operator reads them yet: a WAVE cell
            // CAN be locked to CYCL on a step, and a lock is applied on the audio thread.
            if (id == CycleSample)
                cycles = phaseosc::sampleCycles((int) value);
            for (int op = 0; op < 6; ++op)
                applyWave(op);
            return;
        }
        if (id == PositionEnv)
        {
            for (auto& voice : voices)
                for (int op = 0; op < 6; ++op)
                    voice.getOp(op).setPositionEnv(value);
            return;
        }

        int opIdx = id / 10;
        int paramIdx = id % 10;
        if (opIdx < 0 || opIdx >= 6)
            return;
        if (paramIdx == OP_WAVE)
        {
            applyWave(opIdx);
            return;
        }

        for (auto& voice : voices)
        {
            auto& op = voice.getOp(opIdx);
            switch (paramIdx)
            {
                case 0: op.setRatio(value); break;
                case 1: op.setLevel(value); break;
                case 2: op.setDetune(value); break;
                case 3: op.setFeedback(value); break;
                case 4:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.attack = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 5:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.decay = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 6:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.sustain = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 7:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.release = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case OP_POSITION: op.setPosition(value); break;
                default: break;
            }
        }
    }

    float stored(ParamId id) const
    {
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    // Whether an operator reads the cycles of the SMPL sample.
    bool cyclesInUse() const
    {
        for (int op = 0; op < 6; ++op)
            if ((int) stored(op * 10 + OP_WAVE) == phaseosc::WAVE_CYCLES)
                return true;
        return false;
    }

    // One operator's wave, from its WAVE cell and the cells every operator shares (SMPL, WSEQ,
    // XFAD), on every voice. Nothing is built here (a step lock on WAVE comes this way on the
    // audio thread): the sample's cycles were made when SMPL was set; the voices point at them.
    void applyWave(int op)
    {
        const int index = (int) stored(op * 10 + OP_WAVE);
        phaseosc::Wave wave;
        wave.table = phaseosc::waveTable(index, cycles);
        const int steps = (int) stored(SeqSteps);
        wave.steps = steps > 0 && wave.hasPosition() ? std::min(steps + 1, wave.table->numFrames) : 0;
        wave.fade = stored(SeqFade);
        for (auto& voice : voices)
            voice.getOp(op).setWave(wave);
    }

    // One voice's own value of a page cell (an operator's or the filter's): what a slot routed
    // to that cell sets, once a sub-block. Envelope times act on the stage that is running.
    // (The operators' positions are taken per sample by the voice itself.)
    static void applyVoiceParam(FMVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        if (id < 0 || id >= 60)
            return;
        auto& op = voice.getOp(id / 10);
        switch (id % 10)
        {
            case 0: op.setRatio(value); break;
            case 1: op.setLevel(value); break;
            case 2: op.setDetune(value); break;
            case 3: op.setFeedback(value); break;
            case 4: op.getEnvelope().setAttack(value); break;
            case 5: op.getEnvelope().setDecay(value); break;
            case 6: op.getEnvelope().setSustain(value); break;
            case 7: op.getEnvelope().setRelease(value); break;
            case OP_POSITION: op.setPosition(value); break;
            default: break;
        }
    }

    void updatePitch()
    {
        float mul = std::exp2((tune + bend * bendRange) / 12.0f);
        for (auto& v : voices)
            v.setPitchMul(mul);
    }

    // MIX (operator mixer on knobs 1-6), RATIO, WAVE and WPOS (what each operator reads, and where
    // in it), one page per operator, then the filter's. (The modulation router's pages follow in
    // getParamPages: XIN, the slots, LSHP, STRIP, LFO1, LFO2.)
    static ParamPages build(const char* const* algos)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const char* const lvl[] = { "LVL1", "LVL2", "LVL3", "LVL4", "LVL5", "LVL6" };
        static const char* const rat[] = { "RAT1", "RAT2", "RAT3", "RAT4", "RAT5", "RAT6" };
        static const char* const opNames[] = { "OP1", "OP2", "OP3", "OP4", "OP5", "OP6" };
        static const float defLevel[] = { 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f };

        ParamPages out;

        ParamPage mix; mix.name = "FMMIX"; mix.section = "FM";
        ParamPage ratio; ratio.name = "RATIO"; ratio.section = "FM";
        for (int op = 0; op < 6; ++op)
        {
            mix.params[(size_t) op] = { lvl[op], op * 10 + 1, 0, 1, defLevel[op], L };
            ratio.params[(size_t) op] = { rat[op], op * 10 + 0, 0.5f, 16, 1, L, "ratio" };
        }
        mix.params[6] = { "ALGO", AlgorithmParam, 0, 7, 0, C, "", algos, 8 };
        mix.params[7] = { "LEVL", MasterLevel, 0, 1, 0.8f, L };
        ratio.params[6] = { "TUNE", TuneParam, -24, 24, 0, I, "st" };
        ratio.params[7] = { "BEND", BendRangeParam, 0, 24, 2, I, "st" };
        out.push_back(mix);
        out.push_back(ratio);

        // What the operators read. WAVE: each operator's wave (SINE, the classic operator; CYCL,
        // the cycles of the sample SMPL names, one period at each of 64 places along it; then the
        // built-in wavetables), and WSEQ, which turns every table into a wave sequence: SCAN = the
        // position glides through the table, 2-16 = that many of its waves, each held and then
        // crossfaded into the next. WPOS: each operator's position (an LFO and mod-slot destination),
        // XFAD = how much of a sequence step is crossfade, PENV = the operator's own envelope moves
        // its position.
        static const char* const wav[] = { "WAV1", "WAV2", "WAV3", "WAV4", "WAV5", "WAV6" };
        static const char* const pos[] = { "POS1", "POS2", "POS3", "POS4", "POS5", "POS6" };
        static const char* const seq[MAX_SEQ_STEPS] = { "SCAN", "2", "3", "4", "5", "6", "7", "8", "9", "10",
                                                        "11", "12", "13", "14", "15", "16" };
        auto& bank = SampleBank::get();
        ParamPage wave; wave.name = "WAVE"; wave.section = "FM";
        ParamPage place; place.name = "WPOS"; place.section = "FM";
        for (int op = 0; op < 6; ++op)
        {
            wave.params[(size_t) op] = { wav[op], op * 10 + OP_WAVE, 0, (float) (phaseosc::waveCount() - 1), 0, C, "",
                                         phaseosc::waveLabels(), phaseosc::waveCount() };
            place.params[(size_t) op] = { pos[op], op * 10 + OP_POSITION, 0, 1, 0, L };
        }
        wave.params[6] = { "SMPL", CycleSample, 0, (float) (bank.size() - 1), (float) bank.defaultIndex(), C, "", bank.names(), bank.size() };
        wave.params[7] = { "WSEQ", SeqSteps, 0, (float) (MAX_SEQ_STEPS - 1), 0, C, "", seq, MAX_SEQ_STEPS };
        place.params[6] = { "XFAD", SeqFade, 0, 1, 0.5f, L };
        place.params[7] = { "PENV", PositionEnv, -1, 1, 0, L };
        out.push_back(wave);
        out.push_back(place);

        for (int op = 0; op < 6; ++op)
        {
            ParamPage p; p.name = opNames[op]; p.section = "OPS";
            int b = op * 10;
            p.params = {{
                { "RATI", b + 0, 0.5f, 16, 1, L, "ratio" },
                { "LEVL", b + 1, 0, 1, defLevel[op], L },
                { "DETN", b + 2, -50, 50, 0, L, "ct" },
                { "FDBK", b + 3, 0, 1, 0, L },
                { "ATK",  b + 4, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  b + 5, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  b + 6, 0, 1, 0.7f, L },
                { "REL",  b + 7, 0.001f, 10, 0.3f, G, "s" } }};
            out.push_back(p);
        }

        // VOICE: polyphony. The page goes before the filter's (before STRIP / LFO pages).
        out.push_back({ "VOICE", "VOX", {{ { "VOX", VoiceCountParam, 1, MAX_VOICES, 8, I }, {}, {}, {}, {}, {}, {}, {} }} });

        VoiceFilterSection::appendPages(out, FilterBase);
        return out;
    }

    static constexpr float HEADROOM = 0.7f;
    static constexpr float PHASE_LIMIT = 8.0f;          // radians: what several slots on one operator's phase may add up to
    static constexpr float STRIP_INDEX_SCALE = 2.0f;    // a strip route to INDX at +100 triples the index

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float tune = 0.0f;
    float bend = 0.0f;
    float bendRange = 2.0f;
    float wheel = 0.0f, wheelNow = 0.0f;      // the strip as sent, and smoothed per block
    float legacyXinDepth = 0.5f;              // the old XDPT cell (id 65) as a stored sound last gave it
    const Wavetable* cycles = nullptr;          // the cycles of the SMPL sample (made when SMPL is set, kept for the run)
    VoiceFilterSection filterSection { FilterBase };
    mod::Router router { lfoDests(), 2, 0, &sources() };
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {};
    VoiceAllocator allocator;
    std::vector<FMVoice> voices;
    ParamMap params;
};
