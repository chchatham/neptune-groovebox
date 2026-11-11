#pragma once

#include "mod_wheel_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/EngineLfoSection.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// The LFO destination lists of the seven sound engines.
//
// Every destination of every engine is rendered with an LFO routed to it at full
// depth and compared with the same sound at depth 0, with the spectral
// difference of the "Mod Wheel" tests (0 = the same sound, 1 = nothing in
// common), left and right channel both. A destination that needs something
// switched on to be heard (a filter type that has a drive, a kick-machine pad,
// granular mode) gets it from prepare(); none is skipped. A new destination that
// does nothing fails here.
//
// Also: labels (unique per engine, four characters, described in the manual's
// glossary), the original indices, a floor under each list's length, Func + turn
// on the DEST cell, per-voice routing, and the extra envelopes' longer DEST.
// -----------------------------------------------------------------------------
class LfoDestinationsTests : public juce::UnitTest
{
public:
    LfoDestinationsTests() : UnitTest("LFO Destinations") {}

    using S = EngineLfoSection;
    using W = ModWheelTests;

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr float MIN_DIFFERENCE = 0.02f;

    enum { SIN = 0, SQR = 2 };
    enum { FREE = 0, KEY = 1 };
    enum { SVF = 0, SEM = 1, LADR = 2 };            // FilterSlot's display order

    struct Ev { double at; int note; float velocity; };     // velocity 0 = note off

    struct Plan
    {
        double seconds = 0.6;
        // Struck twice, so a value read at the strike is read at two places of the LFO's cycle.
        std::vector<Ev> events { { 0.0, 48, 0.7f }, { 0.2, 48, 0.0f }, { 0.25, 48, 0.7f }, { 0.45, 48, 0.0f } };
        std::shared_ptr<EngineBase> engine;
        int source = 0;                             // the LFO that is routed to the destination
        juce::String variant;                       // what prepare() set up for this destination in particular
    };

    // ---- the engines ---------------------------------------------------------------------------

    static const std::vector<EngineType>& engineTypes()
    {
        static const std::vector<EngineType> types = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc,
                                                       EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler,
                                                       EngineType::Drum };
        return types;
    }

    static juce::String engineName(EngineType t)
    {
        static const char* const names[] = { "synth", "FM", "macro", "wavetable", "wave seq", "sampler", "drums" };
        return names[(int) t];
    }

    static std::shared_ptr<EngineBase> makeEngine(EngineType type)
    {
        std::shared_ptr<EngineBase> e = AudioEngine::createEngine(type);
        e->prepareToPlay(SR, BLOCK);
        W::makeRepeatable(*e);
        return e;
    }

    // The DEST cell of LFO 1 (the unified voice's is built by the PARAM screen from the same list).
    static std::vector<juce::String> destLabels(EngineBase& e)
    {
        std::vector<juce::String> out;
        if (e.getType() == EngineType::UnifiedVoice)
        {
            for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
                out.push_back(UnifiedVoice::lfoDestLabels()[i]);
            return out;
        }
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
                if (p.id == S::id(0, S::Dest))
                    for (int i = 0; i < p.numChoices; ++i)
                        out.push_back(p.choices[i]);
        return out;
    }

    static int indexOf(const std::vector<juce::String>& labels, const juce::String& label)
    {
        for (size_t i = 0; i < labels.size(); ++i)
            if (labels[i] == label)
                return (int) i;
        return -1;
    }

    static void setLfo(EngineBase& e, int lfo, int dest, float depth, int shape, int trig, float hz)
    {
        if (e.getType() == EngineType::UnifiedVoice)
        {
            if (e.getParameter(ModParam::LFO_COUNT) < (float) (lfo + 1))
                e.setParameter(ModParam::LFO_COUNT, (float) (lfo + 1));
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_RATE), hz);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_SHAPE), (float) shape);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_TRIG), (float) trig);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_SYNC), 0.0f);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_DEPTH), depth);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_DEST), (float) dest);
            return;
        }
        e.setParameter(S::id(lfo, S::Rate), hz);
        e.setParameter(S::id(lfo, S::Shape), (float) shape);
        e.setParameter(S::id(lfo, S::Trigger), (float) trig);
        e.setParameter(S::id(lfo, S::Sync), 0.0f);
        e.setParameter(S::id(lfo, S::Depth), depth);
        e.setParameter(S::id(lfo, S::Dest), (float) dest);
    }

    // ---- what each destination needs to be heard ------------------------------------------------

    static bool isOneOf(const juce::String& label, std::initializer_list<const char*> names)
    {
        for (auto* n : names)
            if (label == n)
                return true;
        return false;
    }

    // The shared voice filter (FM, macro, wavetable, wave sequencer): a filter that is doing
    // something, of a type that has what the destination moves.
    static void prepareVoiceFilter(EngineBase& e, int base, const juce::String& label, juce::String& variant)
    {
        using F = VoiceFilterSection;
        const bool morph = isOneOf(label, { "MORP", "FMOR" }), release = label == "FREL";
        variant << (morph ? "morph " : "") << (release ? "filter release " : "");
        e.setParameter(base + F::Type, (float) (morph ? SEM : LADR));
        e.setParameter(base + F::Mode, 0.0f);
        e.setParameter(base + F::Cutoff, 700.0f);
        e.setParameter(base + F::Resonance, 0.5f);
        e.setParameter(base + F::EnvAmount, 0.7f);
        e.setParameter(base + F::Decay, 0.15f);
        e.setParameter(base + F::Sustain, release ? 0.9f : 0.2f);     // a release needs something to fall from
        e.setParameter(base + F::Drive, 0.3f);
    }

    static Plan prepare(EngineType type, const juce::String& label, const std::vector<juce::String>& labels)
    {
        Plan plan;
        plan.engine = makeEngine(type);
        auto& e = *plan.engine;
        auto set = [&e](int id, float v) { e.setParameter(id, v); };
        const bool otherLfo = isOneOf(label, { "LRAT", "LDPT" });
        const int dest = indexOf(labels, label);
        auto& variant = plan.variant;

        // Another LFO's rate or depth: that LFO is on the level, where it is plainly heard.
        if (otherLfo)
        {
            setLfo(e, 1, indexOf(labels, "AMP"), 0.6f, SIN, FREE, 7.0f);
            variant << "other LFO ";
        }

        switch (type)
        {
            case EngineType::UnifiedVoice:
            {
                using namespace UnifiedParam;
                set(SUB, 0.5f);
                set(NOISE, 0.3f);
                if (label.length() == 4 && (label.startsWith("RAT") || label.startsWith("DPT")))
                {
                    const int target = label.getLastCharacters(1).getIntValue() - 1;
                    plan.source = target == 0 ? 1 : 0;
                    variant << label;
                    setLfo(e, 3, 0, 0.0f, SIN, FREE, 2.0f);                 // four LFOs running
                    setLfo(e, target, (int) UnifiedVoice::LfoDest::Amp, 0.6f, SIN, FREE, 7.0f);
                }
                if (isOneOf(label, { "LVL3", "TUN3" }))
                {
                    variant << "osc 3 ";
                    set(WAVE3, (float) (int) UnifiedVoice::OscWaveform::Saw);
                    set(SUB, 0.9f);
                }
                if (isOneOf(label, { "CUTB", "RESB", "BAL", "DRVB", "TRKB" }))
                {
                    variant << "filter B ";
                    set(MODEL_B, (float) LADR);
                    set(ROUTING, 2.0f);                                     // parallel
                    set(FILTER_B_OFFSET, -12.0f);
                    set(CUTOFF, 900.0f);
                    set(FILTER_ENV, 0.5f);
                    set(RESONANCE_B, 0.5f);
                    set(DRIVE_B, 0.3f);
                }
                if (isOneOf(label, { "FDRV", "MORP" }))
                {
                    variant << label;
                    set(MODEL_A, (float) (label == "MORP" ? SEM : LADR));
                    set(CUTOFF, 900.0f);
                    set(RESONANCE, 0.5f);
                    set(FILTER_DRIVE, 0.3f);
                }
                if (isOneOf(label, { "FATK", "FDEC", "FSUS", "FREL", "KEY", "FVEL", "PMOD", "ENV" }))
                {
                    variant << "filter envelope ";
                    set(CUTOFF, 600.0f);
                    set(FILTER_ENV, 0.5f);
                    set(FENV_DECAY, 0.15f);
                    set(FENV_SUSTAIN, 0.4f);
                }
                if (label.length() == 2 && (label.endsWith("A") || label.endsWith("B")))
                {
                    variant << "FM slots ";
                    for (int id : { WAVE1, WAVE2, WAVE3 })
                        set(id, (float) (int) UnifiedVoice::OscWaveform::FM);
                }
                if (label == "SPRD")
                {
                    variant << "stack ";
                    set(STACK, 1.0f);
                }
                if (label == "GLID")
                {
                    variant << "glide ";
                    set(VOICE_COUNT, 1.0f);
                    set(GLIDE, 0.15f);
                    plan.events = { { 0.0, 48, 0.7f }, { 0.2, 72, 0.7f }, { 0.5, 72, 0.0f } };
                }
                if (isOneOf(label, { "DRFT", "DRAT" }))
                {
                    // Analog character on, with everything that is random per note left at zero.
                    variant << "drift ";
                    auto& u = static_cast<UnifiedVoiceEngine&> (e);
                    for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                        u.getVoice(v).getAnalogCharacter().enabled = true;
                    set(DRIFT, 0.6f);
                    set(DRIFT_RATE, 2.0f);
                    plan.seconds = 2.0;
                    plan.events = { { 0.0, 48, 0.7f }, { 1.8, 48, 0.0f } };
                }
                if (label == "EDPT" || (label.length() == 4 && label.getLastCharacters(1).getIntValue() >= 2
                                        && isOneOf(label.substring(0, 3), { "ATK", "DEC", "SUS", "REL" })))
                {
                    // The four extra envelopes, one after the other, on the pitch.
                    variant << "extra envelopes ";
                    set(ModParam::ENV_COUNT, 4.0f);
                    for (int env = 0; env < 4; ++env)
                    {
                        set(ModParam::envParamId(env, ModParam::ENV_TARGET), 2.0f);
                        set(ModParam::envParamId(env, ModParam::ENV_DEPTH), 0.5f);
                        set(ModParam::envParamId(env, ModParam::ENV_ATK), 0.05f);
                        set(ModParam::envParamId(env, ModParam::ENV_DEC), 0.05f);
                        set(ModParam::envParamId(env, ModParam::ENV_SUS), 0.5f);
                        set(ModParam::envParamId(env, ModParam::ENV_REL), 0.08f);
                    }
                    plan.seconds = 1.0;
                    plan.events = { { 0.0, 48, 0.7f }, { 0.85, 48, 0.0f } };
                }
                break;
            }
            case EngineType::FM:
                for (int op = 0; op < 6; ++op)
                    set(op * 10 + 1, 0.8f);         // every operator in the sound: a bright one
                if (label.length() == 4 && juce::CharacterFunctions::isDigit(label[3]))
                {
                    // An operator's own cell: six carriers, each a partial of its own, so every
                    // operator is heard directly (deep in a chain an operator's envelope is hardly
                    // heard). Higher up the keyboard, where a detune moves a partial by more.
                    variant << "operators ";
                    set(FMEngine::AlgorithmParam, 5.0f);
                    set(FMEngine::MasterLevel, 0.4f);
                    for (int op = 0; op < 6; ++op)
                    {
                        set(op * 10 + 0, (float) (op + 1));
                        set(op * 10 + 1, 0.9f);
                        set(op * 10 + 3, 0.3f);
                        set(op * 10 + 5, 0.1f);
                        set(op * 10 + 6, 0.5f);
                    }
                    plan.events = { { 0.0, 72, 1.0f }, { 0.12, 72, 0.0f }, { 0.25, 72, 1.0f }, { 0.37, 72, 0.0f } };
                }
                prepareVoiceFilter(e, FMEngine::FilterBase, label, variant);
                break;
            case EngineType::MacroOsc:
                // (All four macros act in every model, the default one included: "Dead Controls".)
                prepareVoiceFilter(e, MacroOscEngine::FilterBase, label, variant);
                break;
            case EngineType::Wavetable:
                prepareVoiceFilter(e, WavetableEngine::FilterBase, label, variant);
                set(WavetableEngine::TableSelect, 7.0f);                    // SPEC: every frame is different
                set(WavetableEngine::Position, 0.3f);
                if (label == "MDPT")
                {
                    variant << "motion ";
                    set(WavetableEngine::MorphSpeed, 4.0f);
                }
                if (isOneOf(label, { "TIME", "SPD" }))
                {
                    variant << "scan ";
                    set(WavetableEngine::ScanMode, (float) (int) WavetableScanMode::Loop);
                    set(WavetableEngine::ScanTime, 0.3f);
                }
                if (label == "GLID")
                {
                    // One voice, so the second note takes over the first one's pitch.
                    variant << "glide ";
                    plan.engine = std::make_shared<WavetableEngine>(1);
                    plan.engine->prepareToPlay(SR, BLOCK);
                    plan.engine->setParameter(WavetableEngine::Glide, 0.15f);
                    plan.events = { { 0.0, 48, 0.7f }, { 0.2, 72, 0.7f }, { 0.5, 72, 0.0f } };
                }
                break;
            case EngineType::WaveSeq:
                prepareVoiceFilter(e, WaveSeqEngine::FilterBase, label, variant);
                // All sixteen steps, fast enough to pass every one of them.
                set(WaveSeqEngine::AmpSustain, 0.25f);
                set(WaveSeqEngine::AmpRelease, 0.4f);
                set(WaveSeqEngine::SeqLength, 16.0f);
                for (int id : { (int) WaveSeqEngine::TimingLaneLen, (int) WaveSeqEngine::PitchLaneLen,
                                (int) WaveSeqEngine::ShapeLaneLen, (int) WaveSeqEngine::LevelLaneLen })
                    set(id, 16.0f);
                set(WaveSeqEngine::TempoSync, 0.0f);
                set(WaveSeqEngine::Rate, 4.0f);
                for (int step = 0; step < 16; ++step)
                    set(WaveSeqEngine::StepLevel + step, 0.6f);
                plan.seconds = 0.8;
                plan.events = { { 0.0, 48, 0.7f }, { 0.5, 48, 0.0f } };
                if (label.length() >= 2 && isOneOf(label.substring(0, 1), { "P", "L", "T" })
                    && label.substring(1).containsOnly("0123456789"))
                {
                    // One step of the sequence: the only one that sounds, so what the LFO does to
                    // it is not one sixteenth of the difference.
                    variant << "step " << label.substring(1);
                    const int own = label.substring(1).getIntValue() - 1;
                    for (int step = 0; step < 16; ++step)
                        if (step != own)
                            set(WaveSeqEngine::StepLevel + step, 0.0f);
                    set(WaveSeqEngine::Crossfade, 0.0f);
                    set(WaveSeqEngine::AmpSustain, 1.0f);
                    plan.seconds = 1.25;
                    plan.events = { { 0.0, 48, 0.7f }, { 1.15, 48, 0.0f } };
                }
                break;
            case EngineType::Sampler:
            {
                using E = SamplerEngine;
                set(E::SampleSelect, 0.0f);                                 // BEAT: something happens along it
                set(E::SliceCount, 1.0f);
                set(E::Mode, (float) E::Loop);
                set(E::Sustain, 0.4f);
                set(E::Decay, 0.1f);
                set(E::Release, 0.1f);
                set(E::FilterType, (float) LADR);
                set(E::FilterCutoff, 0.55f);
                set(E::FilterReso, 0.4f);
                set(E::FilterEnv, 0.3f);
                set(E::FilterDrive, 0.3f);
                if (isOneOf(label, { "GPOS", "SIZE", "DENS", "SCAN", "SPRY", "PJIT", "SPRD", "RVRS" }))
                {
                    variant << "grains ";
                    set(E::Mode, (float) E::Granular);
                    set(E::GrainSize, 0.05f);
                    set(E::GrainSpray, 0.2f);
                    set(E::GrainPitchJit, 0.2f);
                    set(E::GrainSpread, 0.3f);
                    set(E::GrainReverse, 0.3f);
                }
                if (isOneOf(label, { "SPD", "SSIZ", "XFAD", "JIT", "SSPR" }))
                {
                    variant << "stretch ";
                    set(E::StretchMode, (float) E::StretchGrain);
                    set(E::StretchSpeed, 0.7f);
                    set(E::StretchJitter, 0.3f);
                    set(E::StretchSpread, 0.3f);
                }
                if (isOneOf(label, { "SLIC", "FADE", "SLEN" }))
                {
                    variant << "slices ";
                    set(E::Mode, (float) E::Forward);
                    set(E::SliceCount, 16.0f);
                    set(E::SliceSelect, 0.4f);
                    set(E::SliceLen, 0.8f);
                    set(E::SliceFade, 0.04f);
                    set(E::Sustain, 1.0f);
                    for (auto& ev : plan.events)
                        ev.note = 60;
                }
                if (isOneOf(label, { "LPST", "LPEN", "END" }))
                {
                    // A short region, played fast and held: the loop comes round many times.
                    variant << "loop ";
                    set(E::EndPoint, 0.04f);
                    set(E::LoopStart, 0.3f);
                    set(E::LoopEnd, 0.7f);
                    set(E::Sustain, 1.0f);
                    for (auto& ev : plan.events)
                        ev.note = 72;
                }
                break;
            }
            case EngineType::Drum:
                // The kick machine (pad 9), struck twice.
                for (auto& ev : plan.events)
                    ev.note = 8;
                break;
            default:
                break;
        }
        return plan;
    }

    // ---- rendering and comparing ---------------------------------------------------------------

    // Left channel, then right channel.
    static std::vector<float> render(const Plan& plan)
    {
        auto& e = *plan.engine;
        const int total = (int) (plan.seconds * SR);
        std::vector<float> l, r;
        l.reserve((size_t) total + BLOCK);
        r.reserve((size_t) total + BLOCK);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        size_t next = 0;
        while ((int) l.size() < total)
        {
            while (next < plan.events.size() && plan.events[next].at * SR <= (double) l.size())
            {
                const auto& ev = plan.events[next++];
                if (ev.velocity > 0.0f) e.noteOn(ev.note, ev.velocity);
                else                    e.noteOff(ev.note);
            }
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
        }
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }

    struct Heard { float difference = 0.0f; bool finite = true; float peak = 0.0f; };

    // The destination with the LFO at +-full depth against depth 0 (the larger of the two).
    // `stills` keeps the depth-0 renders: the destinations with the same set-up share one.
    static Heard hear(EngineType type, const juce::String& label, const std::vector<juce::String>& labels,
                      std::map<juce::String, std::vector<float>>& stills)
    {
        const int dest = indexOf(labels, label);
        Heard h;
        for (float depth : { 1.0f, -1.0f })
        {
            Plan plan = prepare(type, label, labels);
            if (stills.find(plan.variant) == stills.end())
            {
                Plan plain = prepare(type, label, labels);
                setLfo(*plain.engine, plain.source, dest, 0.0f, SQR, FREE, 3.1f);
                stills[plan.variant] = render(plain);
            }
            const auto& still = stills[plan.variant];
            setLfo(*plan.engine, plan.source, dest, depth, SQR, FREE, 3.1f);
            const auto moved = render(plan);
            for (float s : moved)
            {
                h.finite = h.finite && std::isfinite(s);
                h.peak = std::max(h.peak, std::abs(s));
            }
            h.difference = std::max(h.difference, W::difference(still, moved));
            if (h.difference >= 2.0f * MIN_DIFFERENCE)
                break;
        }
        return h;
    }

    // ---- the manual's glossary -----------------------------------------------------------------

    // The short names tools/manual/glossary_data.py describes (the keys of its DESC table). A
    // numbered family is one entry with '#' for the number ("RAT#", "#A").
    static std::set<juce::String> glossaryKeys()
    {
        std::set<juce::String> keys;
        const auto file = juce::File(__FILE__).getParentDirectory().getParentDirectory()
                              .getChildFile("tools/manual/glossary_data.py");
        juce::StringArray lines;
        file.readLines(lines);
        for (auto& line : lines)
        {
            const auto t = line.trimStart();
            if (! t.startsWithChar('"'))
                continue;
            const int end = t.indexOfChar(1, '"');
            if (end > 1 && t.substring(end + 1).trimStart().startsWithChar(':'))
                keys.insert(t.substring(1, end));
        }
        return keys;
    }

    static juce::String numbered(const juce::String& label)
    {
        juce::String out;
        bool inNumber = false;
        for (int i = 0; i < label.length(); ++i)
        {
            const bool digit = juce::CharacterFunctions::isDigit(label[i]);
            if (! digit)            out << label[i];
            else if (! inNumber)    out << '#';
            inNumber = digit;
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------

    void runTest() override
    {
        beginTest("the original destinations keep their indices");
        {
            struct Old { EngineType type; std::vector<const char*> labels; };
            const std::vector<Old> old = {
                { EngineType::UnifiedVoice, { "OFF", "PTCH", "CUT", "RESO", "MIX", "AMP", "PW", "XMOD", "CUTB", "PAN", "LVL1", "LVL2",
                                              "LVL3", "NOIS", "FFM", "PIT2", "RAT1", "RAT2", "RAT3", "RAT4", "DPT1", "DPT2", "DPT3", "DPT4" } },
                { EngineType::FM,        { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "INDX", "FDBK", "DETN" } },
                { EngineType::MacroOsc,  { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "SAWS", "SPRD", "HARM", "MORP" } },
                { EngineType::Wavetable, { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "POS", "SPRD", "SUB" } },
                { EngineType::WaveSeq,   { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "RATE", "XFAD", "PRNG" } },
                { EngineType::Sampler,   { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "STRT", "DRIV", "GPOS", "SIZE", "DENS", "SPD" } },
                { EngineType::Drum,      { "OFF", "PTCH", "DEC", "TONE", "DRIV", "SNAP", "AMP", "PAN" } } };
            for (auto& o : old)
            {
                const auto labels = destLabels(*makeEngine(o.type));
                expect(labels.size() > o.labels.size(), engineName(o.type) + ": the list has grown");
                for (size_t i = 0; i < o.labels.size() && i < labels.size(); ++i)
                    expectEquals(labels[i], juce::String(o.labels[i]), engineName(o.type) + " index " + juce::String((int) i));
            }
            // The enum values projects and the factory presets' mod-strip routes store.
            using D = UnifiedVoice::LfoDest;
            expectEquals((int) D::Pitch, 1);
            expectEquals((int) D::Cutoff, 2);
            expectEquals((int) D::PulseWidth, 6);
            expectEquals((int) D::CrossMod, 7);
            expectEquals((int) D::Lfo4Depth, 23);
            expectEquals((int) D::FirstParam, 24);
        }

        beginTest("the lists are long: a floor under each engine's count");
        {
            const int floors[] = { 75, 69, 27, 34, 75, 42, 18 };
            juce::String counts;
            for (auto type : engineTypes())
            {
                const int n = (int) destLabels(*makeEngine(type)).size();
                counts << engineName(type) << " " << n << "  ";
                expect(n >= floors[(int) type], engineName(type) + " has " + juce::String(n) + " destinations, floor "
                                                    + juce::String(floors[(int) type]));
                expect(n <= S::MAX_DESTS || type == EngineType::UnifiedVoice, engineName(type) + " fits MAX_DESTS");
            }
            logMessage("destinations: " + counts);
        }

        beginTest("labels: unique within an engine, four characters at most, in the manual's glossary");
        {
            const auto glossary = glossaryKeys();
            expect(glossary.size() > 200, "glossary_data.py was read (" + juce::String((int) glossary.size()) + " names)");
            auto check = [&](const juce::String& where, const std::vector<juce::String>& labels)
            {
                std::set<juce::String> seen;
                for (auto& label : labels)
                {
                    expect(seen.insert(label).second, where + ": " + label + " twice");
                    expect(label.isNotEmpty() && label.length() <= 4, where + ": " + label + " does not fit a cell");
                    expect(glossary.count(label) > 0 || glossary.count(numbered(label)) > 0,
                           where + ": " + label + " is not in tools/manual/glossary_data.py");
                }
            };
            for (auto type : engineTypes())
                check(engineName(type), destLabels(*makeEngine(type)));
            // The macro oscillator names its four macros per model.
            for (int model = 0; model < 8; ++model)
            {
                MacroOscEngine e;
                e.setParameter(MacroOscEngine::ModelParam, (float) model);
                check("macro model " + juce::String(model), destLabels(e));
                expectEquals((int) destLabels(e).size(), MacroOscEngine::lfoDests(0).size());
            }
            // The extra envelopes' DEST.
            std::vector<juce::String> env;
            for (int i = 0; i < UnifiedVoice::NUM_ENV_DESTS; ++i)
                env.push_back(UnifiedVoice::envDestLabels()[i]);
            check("synth MENV", env);
            expectEquals(env[0], juce::String("AMP"));
            expectEquals(env[1], juce::String("CUT"));
            expectEquals(env[2], juce::String("PTCH"));
            expectEquals(env[3], juce::String("OFF"));
            // A page cell has the same index in both lists; Func + turn goes by group here too.
            expectEquals(env[(size_t) UnifiedVoice::NUM_FIXED_DESTS], juce::String(UnifiedVoice::PARAM_DESTS[0].label));
            expect(UnifiedVoice::lfoDestOfEnvDest(UnifiedVoice::NUM_FIXED_DESTS) == UnifiedVoice::LfoDest::FirstParam);
            const ParamSpec envDest { "DEST", ModParam::envParamId(0, ModParam::ENV_TARGET), 0, (float) (UnifiedVoice::NUM_ENV_DESTS - 1), 0,
                                      ParamKind::Choice, "", UnifiedVoice::envDestLabels(), UnifiedVoice::NUM_ENV_DESTS };
            expectEquals(envDest.turned(0.0f, 1, TurnStep::Coarse), (float) UnifiedVoice::NUM_ENV_TARGETS);
            expectEquals(envDest.turned(4.0f, 1, TurnStep::Coarse), (float) UnifiedVoice::NUM_FIXED_DESTS);
        }

        beginTest("Func + turn on DEST goes from group to group and reaches both ends");
        for (auto type : engineTypes())
        {
            auto e = makeEngine(type);
            const auto labels = destLabels(*e);
            ParamSpec dest;
            if (type == EngineType::UnifiedVoice)
                dest = { "DEST", ModParam::lfoParamId(0, ModParam::LFO_DEST), 0, (float) (labels.size() - 1), 0,
                         ParamKind::Choice, "", UnifiedVoice::lfoDestLabels(), (int) labels.size() };
            else
                for (auto& page : e->getParamPages())
                    for (auto& p : page.params)
                        if (p.id == S::id(0, S::Dest))
                            dest = p;
            expect(ChoiceGroups::find(dest.choices) != nullptr, engineName(type) + ": the list has groups");
            std::vector<int> stops;
            float at = 0.0f;
            for (int i = 0; i < 40; ++i)
            {
                const float next = dest.turned(at, 1, TurnStep::Coarse);
                if (next == at) break;
                expect(next > at);
                stops.push_back((int) next);
                at = next;
            }
            expectEquals((int) at, (int) labels.size() - 1, engineName(type) + ": the last entry is reached");
            expect(! stops.empty() && stops.front() == 1, engineName(type) + ": first stop is the first destination");
            expect((int) stops.size() >= 3 && (int) stops.size() <= 16,
                   engineName(type) + ": " + juce::String((int) stops.size()) + " stops from OFF to the end");
            // Down again, by the same stops.
            for (int i = (int) stops.size() - 2; i >= 0; --i)
            {
                at = dest.turned(at, -1, TurnStep::Coarse);
                expectEquals((int) at, stops[(size_t) i], engineName(type) + " downwards");
            }
            expectEquals(dest.turned(at, -1, TurnStep::Coarse), 0.0f);
            // A plain turn still moves one entry.
            expectEquals(dest.turned(5.0f, 1, TurnStep::Plain), 6.0f);
        }

        for (auto type : engineTypes())
        {
            beginTest(engineName(type) + ": an LFO at full depth is heard on every destination");
            const auto labels = destLabels(*makeEngine(type));
            juce::String quiet;
            std::map<juce::String, std::vector<float>> stills;
            for (size_t d = 1; d < labels.size(); ++d)
            {
                const Heard h = hear(type, labels[d], labels, stills);
                expect(h.finite, engineName(type) + " " + labels[d] + ": output is finite");
                expect(h.peak < 4.0f, engineName(type) + " " + labels[d] + ": peak " + juce::String(h.peak, 2));
                expect(h.difference >= MIN_DIFFERENCE,
                       engineName(type) + " " + labels[d] + ": difference " + juce::String(h.difference, 4));
                if (h.difference < 3.0f * MIN_DIFFERENCE)
                    quiet << labels[d] << " " << juce::String(h.difference, 3) << "  ";
            }
            if (quiet.isNotEmpty())
                logMessage(engineName(type) + ", smallest differences: " + quiet);
        }

        // One voice's LFO moves that voice only: a note's part of a two-note render is the note
        // played alone (KEY restarts each voice's LFO with its own note, so the second note,
        // struck later, is at another place of the cycle than the first).
        beginTest("per voice: a note is moved by its own LFO, whatever the other notes' LFOs do");
        {
            struct Case { EngineType type; const char* label; int first, second; };
            const std::vector<Case> cases = {
                { EngineType::UnifiedVoice, "DRIV", 48, 67 }, { EngineType::UnifiedVoice, "FDEC", 48, 67 },
                { EngineType::FM, "LVL2", 48, 67 },           { EngineType::FM, "RAT2", 48, 67 },
                { EngineType::MacroOsc, "LEVL", 48, 67 },     { EngineType::Wavetable, "ENVP", 48, 67 },
                { EngineType::WaveSeq, "LEVL", 48, 67 },      { EngineType::WaveSeq, "P3", 48, 67 },
                { EngineType::Sampler, "LEVL", 48, 67 },      { EngineType::Sampler, "ENVF", 48, 67 },
                { EngineType::Drum, "SHAP", 3, 8 } };
            for (auto& c : cases)
            {
                const auto labels = destLabels(*makeEngine(c.type));
                const int dest = indexOf(labels, c.label);
                const juce::String where = engineName(c.type) + " " + c.label;
                expect(dest > 0, where + " exists");
                const double later = 4096.0 / SR;       // whole blocks
                auto run = [&](std::vector<Ev> events, float depth)
                {
                    Plan plan = prepare(c.type, c.label, labels);
                    if (c.type == EngineType::UnifiedVoice)
                    {
                        plan.engine->setParameter(UnifiedParam::MIX_DRIVE, 0.3f);
                        plan.engine->setParameter(UnifiedParam::NOISE, 0.0f);       // one noise generator per voice
                    }
                    plan.events = events;
                    plan.seconds = 0.5;
                    setLfo(*plan.engine, 0, dest, depth, SIN, KEY, 6.0f);
                    return render(plan);
                };
                const auto alone = run({ { later, c.second, 0.7f } }, 0.8f);
                const auto firstOnly = run({ { 0.0, c.first, 0.7f } }, 0.8f);
                const auto both = run({ { 0.0, c.first, 0.7f }, { later, c.second, 0.7f } }, 0.8f);
                const auto aloneStill = run({ { later, c.second, 0.7f } }, 0.0f);
                double err = 0.0, ref = 0.0, moved = 0.0;
                for (size_t i = 0; i < both.size(); ++i)
                {
                    const double part = (double) both[i] - (double) firstOnly[i];
                    err += (part - (double) alone[i]) * (part - (double) alone[i]);
                    ref += (double) alone[i] * (double) alone[i];
                    moved += ((double) alone[i] - (double) aloneStill[i]) * ((double) alone[i] - (double) aloneStill[i]);
                }
                expect(ref > 1.0e-6, where + ": the second note sounds");
                expect(moved > 1.0e-4 * ref, where + ": the LFO moves the note (" + juce::String(std::sqrt(moved / ref), 4) + ")");
                expect(err < 1.0e-6 * ref, where + ": the second note is as it is alone (error "
                                               + juce::String(std::sqrt(err / std::max(ref, 1.0e-12)), 6) + ")");
            }
        }

        beginTest("a cell an LFO lets go of is back at its set value on every voice");
        for (auto type : engineTypes())
        {
            const auto labels = destLabels(*makeEngine(type));
            const size_t firstNew = type == EngineType::UnifiedVoice ? (size_t) UnifiedVoice::NUM_FIXED_DESTS
                                                                     : (size_t) 0;
            Plan used = prepare(type, "PTCH", labels), plain = prepare(type, "PTCH", labels);
            for (Plan* p : { &used, &plain })
                p->seconds = 0.05;
            // Every destination is used for a moment, then the LFO is switched off.
            for (size_t d = std::max<size_t>(1, firstNew); d < labels.size(); ++d)
            {
                setLfo(*used.engine, 0, (int) d, 0.9f, SQR, FREE, 9.0f);
                (void) render(used);
                (void) render(plain);
            }
            setLfo(*used.engine, 0, 0, 0.9f, SQR, FREE, 9.0f);
            used.engine->allNotesOff();
            plain.engine->allNotesOff();
            for (Plan* p : { &used, &plain })
            {
                p->seconds = 0.4;
                p->events = { { 0.0, type == EngineType::Drum ? 8 : 55, 0.7f }, { 0.3, type == EngineType::Drum ? 8 : 55, 0.0f } };
            }
            const float d = W::difference(render(used), render(plain));
            expect(d < 1.0e-3f, engineName(type) + ": plain again, difference " + juce::String(d, 5));
        }

        beginTest("the extra envelopes (MENV) reach the LFOs' destinations, and keep AMP / CUT / PTCH / OFF");
        {
            using namespace UnifiedParam;
            auto run = [](int target, float depth)
            {
                Plan plan;
                plan.engine = makeEngine(EngineType::UnifiedVoice);
                auto& e = *plan.engine;
                e.setParameter(CUTOFF, 900.0f);
                e.setParameter(ModParam::ENV_COUNT, 1.0f);
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_ATK), 0.1f);
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_DEC), 0.1f);
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_SUS), 0.3f);
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_DEPTH), depth);
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), (float) target);
                return render(plan);
            };
            std::vector<juce::String> env;
            for (int i = 0; i < UnifiedVoice::NUM_ENV_DESTS; ++i)
                env.push_back(UnifiedVoice::envDestLabels()[i]);
            const auto off = run(3, 1.0f);
            for (const char* label : { "AMP", "CUT", "PTCH", "PW", "PAN", "XMOD", "DRIV", "TUN2", "FDRV", "DEC" })
            {
                const int target = indexOf(env, label);
                expect(target >= 0, juce::String(label) + " is offered");
                if (juce::String(label) == "FDRV")
                    continue;       // needs a filter type with a drive; covered by the LFO test
                const float d = W::difference(off, run(target, 1.0f));
                expect(d >= MIN_DIFFERENCE, juce::String("envelope to ") + label + ": difference " + juce::String(d, 4));
                // Depth 0 is the sound without the envelope (AMP excepted: the old target's rule).
                if (target >= UnifiedVoice::NUM_ENV_TARGETS)
                    expect(W::difference(off, run(target, 0.0f)) < 1.0e-3f, juce::String("envelope to ") + label + " at depth 0");
            }
            // The four original values mean what they meant.
            UnifiedVoiceEngine e;
            e.setParameter(ModParam::ENV_COUNT, 1.0f);
            for (int t = 0; t < 4; ++t)
            {
                e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), (float) t);
                expectEquals((int) e.getVoice(0).getEnvelopeChain().getTarget(), t);
                expect(e.getVoice(0).getEnvDest() == UnifiedVoice::LfoDest::Off);
            }
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), (float) indexOf(env, "PW"));
            expect(e.getVoice(0).getEnvDest() == UnifiedVoice::LfoDest::PulseWidth);
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), (float) (UnifiedVoice::NUM_ENV_DESTS - 1));
            expectEquals((int) e.getVoice(0).getEnvDest(), (int) UnifiedVoice::LfoDest::Count - 1);
        }

        beginTest("a routed cell follows its knob: the LFO moves around the new set value");
        {
            // FM operator 2 as a carrier, its level under a slow LFO: turning LVL2 (set after the
            // LFO was routed) takes the sound with it.
            auto run = [](float level)
            {
                Plan plan;
                plan.engine = makeEngine(EngineType::FM);
                const auto labels = destLabels(*plan.engine);
                plan.engine->setParameter(FMEngine::AlgorithmParam, 5.0f);
                plan.engine->setParameter(1, 0.0f);
                setLfo(*plan.engine, 0, indexOf(labels, "LVL2"), 0.1f, SIN, FREE, 4.0f);
                plan.engine->setParameter(11, level);
                return W::measure(render(plan)).rms;
            };
            const float low = run(0.2f), high = run(0.8f);
            expect(high > low * 2.5f && high < low * 6.0f,
                   "FM LVL2: level follows the knob, " + juce::String(low, 4) + " -> " + juce::String(high, 4));

            // The synth's mixer drive: the route's set value is the knob's.
            UnifiedVoiceEngine u;
            const auto labels = destLabels(u);
            setLfo(u, 0, indexOf(labels, "DRIV"), 0.5f, SIN, FREE, 4.0f);
            expectEquals(u.getVoice(0).getParamRoute(0), (int) UnifiedParam::MIX_DRIVE);
            expectEquals(u.getVoice(3).getParamRoute(0), (int) UnifiedParam::MIX_DRIVE);
            setLfo(u, 0, (int) UnifiedVoice::LfoDest::Cutoff, 0.5f, SIN, FREE, 4.0f);
            expectEquals(u.getVoice(0).getParamRoute(0), -1);
        }
    }
};

static LfoDestinationsTests lfoDestinationsTests;
