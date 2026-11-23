#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/engine/ModRouter.h"
#include "../src/engine/FMEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/project/FactoryPresets.h"

// -----------------------------------------------------------------------------
// Phase U4-1: what the wave 1 reviews found that belongs in the shared router.
//
// 1. NOTHING ELSE MOVED. Every engine on the router (FM, macro, wavetable, wave sequencer,
//    sampler) rendered before and after, bit for bit (Debug: the pins are the Debug build's,
//    taken from 46fedbc, the tree before this phase): every factory preset at 48 kHz in blocks of
//    64, and five deterministic sounds a engine (plain, LFOs, slots, strip and pad, the track
//    input on a slot) at 44.1 / 48 / 96 kHz in host blocks of 37 and 100 - blocks that are not a
//    whole number of sub-blocks. NEPTUNE_ROUTER_PHASE_DUMP=1 prints the table to pin.
// 2. (a) An engine's audio-rate source is heard a constant sub-block late at any host block.
// 3. (b) A phase destination comes in and is let go without a step, for every engine.
// 4. (c) An input that ends or is shorter than the block is faded by the router.
// 5. (d) phaseosc::readAt: the same samples on the fast path.
// 6. (f) A silent audio slot on the wavetable's POS does not change how the rest of POS is taken.
// -----------------------------------------------------------------------------
class RouterPhaseTests : public juce::UnitTest
{
public:
    RouterPhaseTests() : UnitTest("Router Phase") {}

    using R = mod::Router;
    static juce::String env(const char* name) { return juce::SystemStats::getEnvironmentVariable(name, {}); }

    // ---- Rendering -------------------------------------------------------------------------------
    struct Gesture
    {
        double rate = 48000.0;
        int block = 64;
        double seconds = 1.0;
        float strip = 0.0f;
        float pressure = 0.0f;                  // on the first note's pad, from 0.1 s
        bool input = false;                     // a 330 Hz sine at 0.7 on the track input, whole blocks
    };

    // Three notes at three velocities, overlapping, let go; left then right.
    static std::vector<float> play(EngineBase& e, const Gesture& g)
    {
        e.prepareToPlay(g.rate, g.block);
        e.setModWheel(g.strip);
        const int total = (int) (g.seconds * g.rate);
        std::vector<float> l, r, in((size_t) g.block);
        l.reserve((size_t) total * 2);
        r.reserve((size_t) total);
        juce::AudioBuffer<float> buf(2, g.block);
        juce::MidiBuffer midi;
        const auto at = [&g](double s) { return (long) (s * g.rate); };
        bool on1 = false, on2 = false, on3 = false, off1 = false, off23 = false, pressed = false;
        for (long done = 0; done < total; done += g.block)
        {
            if (! on1 && done >= 0)                 { e.noteOn(48, 0.9f, -1); on1 = true; }
            if (! pressed && g.pressure > 0.0f && done >= at(0.1)) { e.aftertouch(48, g.pressure); pressed = true; }
            if (! on2 && done >= at(0.15))          { e.noteOn(60, 0.6f, -1); on2 = true; }
            if (! on3 && done >= at(0.3))           { e.noteOn(67, 1.0f, -1); on3 = true; }
            if (! off1 && done >= at(0.5))          { e.noteOff(48, -1); off1 = true; }
            if (! off23 && done >= at(0.6))         { e.noteOff(60, -1); e.noteOff(67, -1); off23 = true; }
            if (g.input)
            {
                for (int i = 0; i < g.block; ++i)
                    in[(size_t) i] = 0.7f * (float) std::sin(6.283185307179586 * 330.0 * (double) (done + i) / g.rate);
                e.setAudioInput({ in.data(), in.data(), g.block, 0.5f });
            }
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < g.block; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
        }
        e.setAudioInput({});
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }

    static uint32_t hash(const std::vector<float>& x)
    {
        uint32_t h = 2166136261u;
        for (float s : x)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &s, sizeof(bits));
            for (int k = 0; k < 4; ++k)
            {
                h ^= (bits >> (8 * k)) & 0xffu;
                h *= 16777619u;
            }
        }
        return h;
    }

    static std::vector<EngineType> routerEngines()
    {
        return { EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler };
    }
    static juce::String name(EngineType t)
    {
        switch (t)
        {
            case EngineType::FM:        return "FM";
            case EngineType::MacroOsc:  return "macro";
            case EngineType::Wavetable: return "wavetable";
            case EngineType::WaveSeq:   return "wave seq";
            case EngineType::Sampler:   return "sampler";
            default:                    return "other";
        }
    }

    // The engine's DEST list (LFO 1's DEST cell) and the index of a label in it.
    static int destIndex(EngineBase& e, const char* label)
    {
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
                if (p.id == R::lfoId(0, R::DestCell))
                    for (int i = 0; i < p.numChoices; ++i)
                        if (juce::String(p.choices[i]) == label)
                            return i;
        return -1;
    }
    static int sourcePlace(EngineBase& e, const char* label)
    {
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
                if (p.id == ModSlotParam::id(0, ModSlotParam::SRC))
                    for (int i = 0; i < p.numChoices; ++i)
                        if (juce::String(p.choices[i]) == label)
                            return i;
        return -1;
    }

    // The five deterministic sounds of the grid.
    static constexpr int NUM_GRID_SOUNDS = 5;
    static const char* gridSoundName(int k)
    {
        static const char* const names[NUM_GRID_SOUNDS] = { "plain", "LFO1 > PTCH, LFO2 > CUT", "slots ENV > CUT, KEY > AMP",
                                                            "strip up, pad pressed", "input: XIN > AMP, XLP > CUT" };
        return names[k];
    }
    static void gridSound(EngineBase& e, int k, Gesture& g)
    {
        switch (k)
        {
            case 1:
                e.setParameter(R::lfoId(0, R::Rate), 4.0f);
                e.setParameter(R::lfoId(0, R::DestCell), (float) destIndex(e, "PTCH"));
                e.setParameter(R::lfoId(0, R::Depth), 0.4f);
                e.setParameter(R::lfoId(1, R::Rate), 1.3f);
                e.setParameter(R::lfoId(1, R::Shape), 2.0f);
                e.setParameter(R::lfoId(1, R::DestCell), (float) destIndex(e, "CUT"));
                e.setParameter(R::lfoId(1, R::Depth), -0.5f);
                break;
            case 2:
                e.setParameter(ModSlotParam::id(0, ModSlotParam::SRC), (float) sourcePlace(e, "ENV"));
                e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), (float) destIndex(e, "CUT"));
                e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.5f);
                e.setParameter(ModSlotParam::id(1, ModSlotParam::SRC), (float) sourcePlace(e, "KEY"));
                e.setParameter(ModSlotParam::id(1, ModSlotParam::DEST), (float) destIndex(e, "AMP"));
                e.setParameter(ModSlotParam::id(1, ModSlotParam::AMT), -0.3f);
                break;
            case 3:
                g.strip = 0.8f;
                g.pressure = 0.6f;
                break;
            case 4:
                g.input = true;
                e.setParameter(R::xinId(R::XinDest1), (float) destIndex(e, "AMP"));
                e.setParameter(R::xinId(R::XinAmount1), 0.5f);
                e.setParameter(R::xinId(R::XinDest2), (float) destIndex(e, "CUT"));
                e.setParameter(R::xinId(R::XinAmount2), 0.4f);
                e.setParameter(R::xinId(R::XinFilter2), 1.0f);
                break;
            default:
                break;
        }
    }

    // Every case of the grid with its render's hash, in a fixed order.
    struct GridCase { juce::String key; uint32_t hash = 0; float peak = 0.0f; };
    static std::vector<GridCase> grid()
    {
        std::vector<GridCase> out;
        auto add = [&out](const juce::String& key, const std::vector<float>& x)
        {
            float peak = 0.0f;
            for (float s : x)
                peak = std::max(peak, std::abs(s));
            out.push_back({ key, hash(x), peak });
        };
        for (auto type : routerEngines())
        {
            int n = 0;
            for (auto& p : FactoryPresets::getAll())
            {
                if (p.engineType != type)
                    continue;
                auto e = AudioEngine::createEngine(type);
                for (auto& kv : p.soundParams)
                    if (EngineParamRange::contains(kv.first))
                        e->setParameter(kv.first, kv.second);
                Gesture g;
                g.seconds = 0.8;
                add(name(type) + " preset " + juce::String(n++) + " " + p.name, play(*e, g));
            }
            for (int k = 0; k < NUM_GRID_SOUNDS; ++k)
                for (double rate : { 44100.0, 48000.0, 96000.0 })
                    for (int block : { 37, 100 })
                    {
                        auto e = AudioEngine::createEngine(type);
                        Gesture g;
                        g.rate = rate;
                        g.block = block;
                        gridSound(*e, k, g);
                        add(name(type) + " " + gridSoundName(k) + " " + juce::String((int) rate) + "/" + juce::String(block), play(*e, g));
                    }
        }
        return out;
    }

    // The hashes of 8571540's Debug build (re-pinned: U5 pressure slots changed the
    // "strip up, pad pressed" renders; U6 added fusion and resonator presets).
    static const std::map<juce::String, uint32_t>& pins()
    {
        static const std::map<juce::String, uint32_t> table = {
            { "FM preset 0 FM Bass 2", 0x9a6bda85u },
            { "FM preset 1 FM Bell Pad", 0x9a99bf91u },
            { "FM preset 2 Anthem Bell", 0xe001f465u },
            { "FM preset 3 FM Pluck", 0xd7992521u },
            { "FM preset 4 Electric Piano", 0x9a24f8d5u },
            { "FM preset 5 Rhodes", 0x66089ac5u },
            { "FM preset 6 FM Metallic", 0xd4ebe4e1u },
            { "FM preset 7 Glass Marimba", 0x68837e55u },
            { "FM preset 8 Metallic Stab", 0xf1c7cb21u },
            { "FM preset 9 FM Pad", 0x9f3a4c95u },
            { "FM plain 44100/37", 0x9e849fa1u },
            { "FM plain 44100/100", 0xb726055du },
            { "FM plain 48000/37", 0xa15d7699u },
            { "FM plain 48000/100", 0x96c8fbe5u },
            { "FM plain 96000/37", 0x6b225badu },
            { "FM plain 96000/100", 0xdc7577e9u },
            { "FM LFO1 > PTCH, LFO2 > CUT 44100/37", 0x82eef4c5u },
            { "FM LFO1 > PTCH, LFO2 > CUT 44100/100", 0x8b3dbe1du },
            { "FM LFO1 > PTCH, LFO2 > CUT 48000/37", 0x2cb393cdu },
            { "FM LFO1 > PTCH, LFO2 > CUT 48000/100", 0xc1c6e791u },
            { "FM LFO1 > PTCH, LFO2 > CUT 96000/37", 0x75241745u },
            { "FM LFO1 > PTCH, LFO2 > CUT 96000/100", 0xa55bb889u },
            { "FM slots ENV > CUT, KEY > AMP 44100/37", 0x320027d5u },
            { "FM slots ENV > CUT, KEY > AMP 44100/100", 0x38bd1481u },
            { "FM slots ENV > CUT, KEY > AMP 48000/37", 0xfc60ff0du },
            { "FM slots ENV > CUT, KEY > AMP 48000/100", 0xe4a5d8bdu },
            { "FM slots ENV > CUT, KEY > AMP 96000/37", 0x37280c15u },
            { "FM slots ENV > CUT, KEY > AMP 96000/100", 0xa23cade5u },
            { "FM strip up, pad pressed 44100/37", 0xdfe98f41u },
            { "FM strip up, pad pressed 44100/100", 0xdd5cb529u },
            { "FM strip up, pad pressed 48000/37", 0xb1af809du },
            { "FM strip up, pad pressed 48000/100", 0xd9836421u },
            { "FM strip up, pad pressed 96000/37", 0x3561ad39u },
            { "FM strip up, pad pressed 96000/100", 0x4bbac9c9u },
            { "FM input: XIN > AMP, XLP > CUT 44100/37", 0xfe737fd5u },
            { "FM input: XIN > AMP, XLP > CUT 44100/100", 0x12979b25u },
            { "FM input: XIN > AMP, XLP > CUT 48000/37", 0x2a56e931u },
            { "FM input: XIN > AMP, XLP > CUT 48000/100", 0x5f37caa1u },
            { "FM input: XIN > AMP, XLP > CUT 96000/37", 0xea5ce639u },
            { "FM input: XIN > AMP, XLP > CUT 96000/100", 0xb871c1edu },
            { "macro preset 0 Grain Cloud", 0xe4b43045u },
            { "macro preset 1 Resonator Pluck", 0x6fd54d9du },
            { "macro preset 2 Super Saw Lead", 0xbb356cb1u },
            { "macro preset 3 Hollow Sq Bass", 0xf025b491u },
            { "macro preset 4 Soft Sine Pad", 0x8db8c669u },
            { "macro preset 5 Octave Strings", 0xbec1e291u },
            { "macro preset 6 FM Glass Keys", 0x695797e5u },
            { "macro preset 7 FM Growl Bass", 0x08d49245u },
            { "macro preset 8 Detuned Chime", 0x50508911u },
            { "macro preset 9 Feedback Scream", 0x0037e461u },
            { "macro preset 10 Fold Lead", 0x6baef399u },
            { "macro preset 11 West Coast Bass", 0x1276c439u },
            { "macro preset 12 Cheby Organ", 0xf41e34e5u },
            { "macro preset 13 Fold Sweep Pad", 0xd1c27befu },
            { "macro preset 14 Choir Aah", 0x2a6561f9u },
            { "macro preset 15 Talking Lead", 0x547b7635u },
            { "macro preset 16 Whisper Wind", 0x95558ff5u },
            { "macro preset 17 Vox Bass", 0x6494bf65u },
            { "macro preset 18 Pure Organ", 0x844e5b0du },
            { "macro preset 19 Stiff Bell", 0x6475cf81u },
            { "macro preset 20 Clarinet Tone", 0xc9df83bdu },
            { "macro preset 21 Glass Harmonics", 0x2ec6c3c5u },
            { "macro preset 22 Nylon Guitar", 0x4115da0du },
            { "macro preset 23 Pluck Bass", 0xbdc6f355u },
            { "macro preset 24 Koto Snap", 0x2b95a185u },
            { "macro preset 25 String Drum", 0x64e1f979u },
            { "macro preset 26 Vinyl Crackle", 0xf4c2b921u },
            { "macro preset 27 Pitched Rain", 0xcb31483du },
            { "macro preset 28 Hiss Riser", 0x84d93ca5u },
            { "macro preset 29 Dust Hat", 0x0819a6e1u },
            { "macro preset 30 Solo Cello", 0x3b404b2du },
            { "macro preset 31 Glass Bow", 0x734ab4f5u },
            { "macro preset 32 Scrape FX", 0x3f11f189u },
            { "macro preset 33 Bowed Bass", 0xe5246259u },
            { "macro plain 44100/37", 0xc7cdb435u },
            { "macro plain 44100/100", 0x13fd7bb9u },
            { "macro plain 48000/37", 0x00b1a6e5u },
            { "macro plain 48000/100", 0x6a5466a5u },
            { "macro plain 96000/37", 0x947927bdu },
            { "macro plain 96000/100", 0x6dfd7d71u },
            { "macro LFO1 > PTCH, LFO2 > CUT 44100/37", 0x13adeb05u },
            { "macro LFO1 > PTCH, LFO2 > CUT 44100/100", 0x3c170465u },
            { "macro LFO1 > PTCH, LFO2 > CUT 48000/37", 0x244e45f5u },
            { "macro LFO1 > PTCH, LFO2 > CUT 48000/100", 0xda78d0f5u },
            { "macro LFO1 > PTCH, LFO2 > CUT 96000/37", 0x8e4ed22du },
            { "macro LFO1 > PTCH, LFO2 > CUT 96000/100", 0x0d6fad45u },
            { "macro slots ENV > CUT, KEY > AMP 44100/37", 0x85315e2du },
            { "macro slots ENV > CUT, KEY > AMP 44100/100", 0x0cd8c5c1u },
            { "macro slots ENV > CUT, KEY > AMP 48000/37", 0xbc3aad65u },
            { "macro slots ENV > CUT, KEY > AMP 48000/100", 0x79704105u },
            { "macro slots ENV > CUT, KEY > AMP 96000/37", 0x8926d285u },
            { "macro slots ENV > CUT, KEY > AMP 96000/100", 0x8a2f64bdu },
            { "macro strip up, pad pressed 44100/37", 0xe7018a61u },
            { "macro strip up, pad pressed 44100/100", 0x1e637ea5u },
            { "macro strip up, pad pressed 48000/37", 0x8da0d05du },
            { "macro strip up, pad pressed 48000/100", 0x42717555u },
            { "macro strip up, pad pressed 96000/37", 0xd5a2cd6du },
            { "macro strip up, pad pressed 96000/100", 0xd28ee3e5u },
            { "macro input: XIN > AMP, XLP > CUT 44100/37", 0x6ed98c4du },
            { "macro input: XIN > AMP, XLP > CUT 44100/100", 0xdb26ed11u },
            { "macro input: XIN > AMP, XLP > CUT 48000/37", 0x3e15a499u },
            { "macro input: XIN > AMP, XLP > CUT 48000/100", 0x0aa038f5u },
            { "macro input: XIN > AMP, XLP > CUT 96000/37", 0xe631ac65u },
            { "macro input: XIN > AMP, XLP > CUT 96000/100", 0xc1098c69u },
            { "wavetable preset 0 Noise Sweep", 0xf7d6e3b9u },
            { "wavetable preset 1 Wind", 0x3c446585u },
            { "wavetable preset 2 Digital Choir", 0x4663156du },
            { "wavetable preset 3 Frozen Sweep", 0xf5b2cbf5u },
            { "wavetable preset 4 Glitch Table", 0x81c8d3a1u },
            { "wavetable preset 5 Vowel Lead", 0xf6ca0e25u },
            { "wavetable preset 6 Vowel Choir", 0x73225b21u },
            { "wavetable preset 7 I You We Yeah", 0x228ab6b5u },
            { "wavetable preset 8 Sync Sweep Lead", 0xb517d4bdu },
            { "wavetable preset 9 Fold Bass", 0xe4318415u },
            { "wavetable preset 10 FM Table Keys", 0x8e1bc381u },
            { "wavetable preset 11 Glass Pluck", 0x27a38d05u },
            { "wavetable preset 12 Struck Bar", 0x3bbb2865u },
            { "wavetable preset 13 Drawbar Organ", 0xcaf79239u },
            { "wavetable preset 14 Noise To Tone", 0x286876d1u },
            { "wavetable preset 15 Reso Sweep Bass", 0xe32e0a8du },
            { "wavetable preset 16 Reso Sweep Pad", 0x631327fdu },
            { "wavetable preset 17 Walsh Stepper", 0x9e95451du },
            { "wavetable preset 18 Prime Shimmer", 0x4310e94du },
            { "wavetable preset 19 Octave Climb", 0x6e844c51u },
            { "wavetable preset 20 Phase Bass", 0xb2ebf125u },
            { "wavetable preset 21 Comb Phase Pad", 0xf68750e5u },
            { "wavetable preset 22 Eight Bit Vowels", 0xe17068d9u },
            { "wavetable preset 23 Glassy Bell", 0xfb200275u },
            { "wavetable preset 24 Raw Sync Lead", 0xfdb411edu },
            { "wavetable plain 44100/37", 0xec163e99u },
            { "wavetable plain 44100/100", 0x4388898du },
            { "wavetable plain 48000/37", 0xae92a54du },
            { "wavetable plain 48000/100", 0x134e4ef9u },
            { "wavetable plain 96000/37", 0x1da2dd05u },
            { "wavetable plain 96000/100", 0xc6e31d55u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 44100/37", 0xcbe95249u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 44100/100", 0xe0cc5bb5u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 48000/37", 0x57c40ac5u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 48000/100", 0x5428e315u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 96000/37", 0xf055b6a1u },
            { "wavetable LFO1 > PTCH, LFO2 > CUT 96000/100", 0xca1d26a1u },
            { "wavetable slots ENV > CUT, KEY > AMP 44100/37", 0x4bc2fe85u },
            { "wavetable slots ENV > CUT, KEY > AMP 44100/100", 0x080a36e1u },
            { "wavetable slots ENV > CUT, KEY > AMP 48000/37", 0xad7322edu },
            { "wavetable slots ENV > CUT, KEY > AMP 48000/100", 0x43def8ddu },
            { "wavetable slots ENV > CUT, KEY > AMP 96000/37", 0xb413e639u },
            { "wavetable slots ENV > CUT, KEY > AMP 96000/100", 0x01a191f5u },
            { "wavetable strip up, pad pressed 44100/37", 0x93c953f5u },
            { "wavetable strip up, pad pressed 44100/100", 0x88b16e85u },
            { "wavetable strip up, pad pressed 48000/37", 0x0779b0a9u },
            { "wavetable strip up, pad pressed 48000/100", 0x1fffdbe5u },
            { "wavetable strip up, pad pressed 96000/37", 0x7eb89a25u },
            { "wavetable strip up, pad pressed 96000/100", 0xbfdbb165u },
            { "wavetable input: XIN > AMP, XLP > CUT 44100/37", 0xa92d8b75u },
            { "wavetable input: XIN > AMP, XLP > CUT 44100/100", 0xfb8f2af5u },
            { "wavetable input: XIN > AMP, XLP > CUT 48000/37", 0x41337e99u },
            { "wavetable input: XIN > AMP, XLP > CUT 48000/100", 0x66353185u },
            { "wavetable input: XIN > AMP, XLP > CUT 96000/37", 0x4d99a325u },
            { "wavetable input: XIN > AMP, XLP > CUT 96000/100", 0x33943075u },
            { "wave seq preset 0 Sequenced Bells", 0x5286f57du },
            { "wave seq preset 1 Morphing Texture", 0x931194e5u },
            { "wave seq plain 44100/37", 0x147353a1u },
            { "wave seq plain 44100/100", 0xa6313845u },
            { "wave seq plain 48000/37", 0x05450299u },
            { "wave seq plain 48000/100", 0x7c4dcbadu },
            { "wave seq plain 96000/37", 0x70928fc5u },
            { "wave seq plain 96000/100", 0xc648e339u },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 44100/37", 0xb5377021u },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 44100/100", 0xd357f8a5u },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 48000/37", 0xfe7b60fdu },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 48000/100", 0xb9fe8ba1u },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 96000/37", 0xfb401e99u },
            { "wave seq LFO1 > PTCH, LFO2 > CUT 96000/100", 0x99a41fd5u },
            { "wave seq slots ENV > CUT, KEY > AMP 44100/37", 0x0a207fd5u },
            { "wave seq slots ENV > CUT, KEY > AMP 44100/100", 0xffa8a0b5u },
            { "wave seq slots ENV > CUT, KEY > AMP 48000/37", 0xd3755915u },
            { "wave seq slots ENV > CUT, KEY > AMP 48000/100", 0x96a71d35u },
            { "wave seq slots ENV > CUT, KEY > AMP 96000/37", 0x69b4f115u },
            { "wave seq slots ENV > CUT, KEY > AMP 96000/100", 0xde708b05u },
            { "wave seq strip up, pad pressed 44100/37", 0x0f9f5bd9u },
            { "wave seq strip up, pad pressed 44100/100", 0x5cd26531u },
            { "wave seq strip up, pad pressed 48000/37", 0x99074969u },
            { "wave seq strip up, pad pressed 48000/100", 0xc1f35f81u },
            { "wave seq strip up, pad pressed 96000/37", 0x9749e805u },
            { "wave seq strip up, pad pressed 96000/100", 0x390fc251u },
            { "wave seq input: XIN > AMP, XLP > CUT 44100/37", 0x91e56c6du },
            { "wave seq input: XIN > AMP, XLP > CUT 44100/100", 0x5a73bc69u },
            { "wave seq input: XIN > AMP, XLP > CUT 48000/37", 0x453f4831u },
            { "wave seq input: XIN > AMP, XLP > CUT 48000/100", 0x4fcd9ef5u },
            { "wave seq input: XIN > AMP, XLP > CUT 96000/37", 0x86138cadu },
            { "wave seq input: XIN > AMP, XLP > CUT 96000/100", 0xe6507a91u },
            { "sampler preset 0 Lo-Fi Tape", 0x1c2778c5u },
            { "sampler preset 1 Abyss Drone", 0xcf4b3eb4u },
            { "sampler preset 2 Granular Pad", 0x3e7ab945u },
            { "sampler preset 3 Vinyl Scratch", 0x68dbd0b5u },
            { "sampler preset 4 Break 100 Loop", 0x1f9bb7d1u },
            { "sampler preset 5 Break 100 Slices", 0xc674fd7du },
            { "sampler preset 6 Break 170 Loop", 0x97d30b45u },
            { "sampler preset 7 Break 170 Slices", 0xc27957cdu },
            { "sampler preset 8 Break 80 Loop", 0x020a13e1u },
            { "sampler preset 9 Break 80 Slices", 0x2232807du },
            { "sampler preset 10 Break 90 Loop", 0xd0606f14u },
            { "sampler preset 11 Break 90 Slices", 0x7946624du },
            { "sampler preset 12 Break 105 Loop", 0xd9e2980au },
            { "sampler preset 13 Break 105 Slices", 0x32f550c5u },
            { "sampler preset 14 Break 120 Loop", 0x568f56f1u },
            { "sampler preset 15 Break 120 Slices", 0x10b75291u },
            { "sampler preset 16 Break 157 Loop", 0x19248f55u },
            { "sampler preset 17 Break 157 Slices", 0x78a628d0u },
            { "sampler preset 18 Break 160 Loop", 0x76173886u },
            { "sampler preset 19 Break 160 Slices", 0xfa8a93e2u },
            { "sampler plain 44100/37", 0xdce8d331u },
            { "sampler plain 44100/100", 0xf84e4c0du },
            { "sampler plain 48000/37", 0x66fb525du },
            { "sampler plain 48000/100", 0x5dbec279u },
            { "sampler plain 96000/37", 0x02418061u },
            { "sampler plain 96000/100", 0x91e5b5e9u },
            { "sampler LFO1 > PTCH, LFO2 > CUT 44100/37", 0x192a4bc5u },
            { "sampler LFO1 > PTCH, LFO2 > CUT 44100/100", 0xf19ddbc5u },
            { "sampler LFO1 > PTCH, LFO2 > CUT 48000/37", 0x806e470du },
            { "sampler LFO1 > PTCH, LFO2 > CUT 48000/100", 0x999c51a5u },
            { "sampler LFO1 > PTCH, LFO2 > CUT 96000/37", 0x8bb3ef6du },
            { "sampler LFO1 > PTCH, LFO2 > CUT 96000/100", 0x5fa485a5u },
            { "sampler slots ENV > CUT, KEY > AMP 44100/37", 0xcecb9f8du },
            { "sampler slots ENV > CUT, KEY > AMP 44100/100", 0x25c5ba05u },
            { "sampler slots ENV > CUT, KEY > AMP 48000/37", 0x2c78f731u },
            { "sampler slots ENV > CUT, KEY > AMP 48000/100", 0xb06619c1u },
            { "sampler slots ENV > CUT, KEY > AMP 96000/37", 0xda8b7dbdu },
            { "sampler slots ENV > CUT, KEY > AMP 96000/100", 0x782daf69u },
            { "sampler strip up, pad pressed 44100/37", 0x210d9261u },
            { "sampler strip up, pad pressed 44100/100", 0x24f7c505u },
            { "sampler strip up, pad pressed 48000/37", 0x27896521u },
            { "sampler strip up, pad pressed 48000/100", 0xc7203595u },
            { "sampler strip up, pad pressed 96000/37", 0xc2b2b73du },
            { "sampler strip up, pad pressed 96000/100", 0xb611282du },
            { "sampler input: XIN > AMP, XLP > CUT 44100/37", 0x17d48d7du },
            { "sampler input: XIN > AMP, XLP > CUT 44100/100", 0x339a8695u },
            { "sampler input: XIN > AMP, XLP > CUT 48000/37", 0x75f04bcdu },
            { "sampler input: XIN > AMP, XLP > CUT 48000/100", 0x23505891u },
            { "sampler input: XIN > AMP, XLP > CUT 96000/37", 0x10262ee1u },
            { "sampler input: XIN > AMP, XLP > CUT 96000/100", 0x3e1a1a59u },
        };
        return table;
    }

    // ---- A router by itself: one voice over a table of test destinations ------------------------
    struct Bench
    {
        enum { X = 1, PH, RAD, C, NEG };    // per sample; a phase in cycles; a phase in radians; control rate; a phase with a negative span
        enum { MADE = 0, NOW = 1 };     // the published sources: made by the voice; Immediate
        static const mod::DestTable& table()
        {
            static const mod::DestTable t = []
            {
                mod::DestTable d;
                d.group();
                d.input("X", 0.0f, 1.0f, -8.0f, 8.0f, mod::PerSample);
                d.input("PH", 0.0f, 1.0f, -8.0f, 8.0f, mod::PerSample | mod::Phase);
                d.input("RAD", 0.0f, 1.0f, -8.0f, 8.0f, mod::PerSample | mod::Phase).period(dsp::TWO_PI_F);
                d.input("C", 0.0f, 1.0f, -1.0f, 1.0f);
                d.input("NEG", 0.0f, -1.0f, -8.0f, 8.0f, mod::PerSample | mod::Phase);     // (the U4-1 review)
                d.finish(2);
                return d;
            }();
            return t;
        }
        static const mod::SourceTable& sources()
        {
            static const mod::SourceTable t = []
            {
                mod::SourceTable s(2);
                s.publish("MADE", mod::AudioRate);
                s.publish("NOW", mod::AudioRate | mod::Immediate);
                return s;
            }();
            return t;
        }

        mod::Router r { table(), 2, 0, &sources() };
        mod::Voice v;
        double sr;
        int block;
        long long clock = 0;
        // The track input: a value per sample, and how many frames of each block are there (none: all).
        std::function<float(long long)> input;
        std::function<int(long long, int)> frames;
        std::vector<float> in;

        Bench(double rate, int hostBlock) : sr(rate), block(hostBlock)
        {
            r.prepare(sr);
            v.prepare(sr);
            v.seed(1, 0);
        }
        void slot(int k, int source, int dest, float amount)
        {
            r.set(ModSlotParam::id(k, ModSlotParam::SRC), (float) r.sourceIndex(source));
            r.set(ModSlotParam::id(k, ModSlotParam::DEST), (float) dest);
            r.set(ModSlotParam::id(k, ModSlotParam::AMT), amount);
        }
        // `samples` samples in host blocks: before(t, n) before each tick, after(t, n) after it.
        template <typename Before, typename After>
        void run(long long samples, Before&& before, After&& after)
        {
            in.resize((size_t) block);
            for (long long done = 0; done < samples;)
            {
                const int len = (int) std::min<long long>(block, samples - done);
                int have = 0;
                if (input)
                {
                    have = frames ? std::clamp(frames(clock, len), 0, len) : len;
                    for (int i = 0; i < len; ++i)
                        in[(size_t) i] = i < have ? input(clock + i) : 1.0e3f;     // (beyond the frames: never read)
                }
                r.beginBlock(have > 0 ? in.data() : nullptr, have > 0 ? in.data() : nullptr, have, 0.0f, 0.0f, sr);
                for (int off = 0; off < len;)
                {
                    const int n = r.beginChunk(off, len);
                    before(clock, n);
                    v.tick(r, n, 1.0f, 1.0f, 0.0f);
                    after(clock, n);
                    off += n;
                    clock += n;
                }
                done += len;
            }
        }
        template <typename After>
        void run(long long samples, After&& after) { run(samples, [](long long, int) {}, after); }
        // The per-sample values of a destination for the sub-block just ticked (0 where nothing moves it).
        void collect(int dest, int n, std::vector<float>& out) const
        {
            const float* b = v.buffer(dest);
            for (int i = 0; i < n; ++i)
                out.push_back(b != nullptr ? b[i] : 0.0f);
        }
    };

    // What the voice "makes": a value for every sample, nearly all different.
    static float madeAt(long long t)
    {
        return 0.6f * (float) std::sin(0.37 * (double) t) * (float) (1.0 - 0.5 * std::cos(0.0011 * (double) t));
    }

    // The largest change from one sample to the next, a whole period being none.
    static double largestTurn(const std::vector<float>& h, size_t from, size_t to, double period)
    {
        double worst = 0.0;
        for (size_t i = std::max<size_t>(from, 1); i < std::min(to, h.size()); ++i)
        {
            double d = (double) h[i] - (double) h[i - 1];
            d -= period * std::round(d / period);
            worst = std::max(worst, std::abs(d));
        }
        return worst;
    }

    static float largestStep(const std::vector<float>& y, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < std::min(to, y.size()); ++i)
            m = std::max(m, std::abs(y[i] - y[i - 1]));
        return m;
    }

    // ---- An engine under a held note, cells set between blocks ------------------------------------
    struct Live
    {
        std::shared_ptr<EngineBase> e;
        double sr;
        int block;
        long long clock = 0;
        std::function<float(long long)> input;      // per sample (none: nothing connected)
        std::function<int(long long, int)> frames;  // of each block (none: all)
        std::vector<float> in;
        AftertouchRouter off;
        Live(EngineType type, double rate, int hostBlock) : e(AudioEngine::createEngine(type)), sr(rate), block(hostBlock)
        {
            off.clear();
            e->setAftertouchRouter(&off);
            e->prepareToPlay(sr, block);
            in.resize((size_t) block);
        }
        ~Live() { e->setAftertouchRouter(nullptr); }
        void set(const std::vector<std::pair<int, float>>& cells) { for (auto& c : cells) e->setParameter(c.first, c.second); }
        std::vector<float> run(double seconds)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> buf(2, block);
            juce::MidiBuffer midi;
            const long long total = (long long) std::llround(seconds * sr);
            for (long long done = 0; done < total; done += block)
            {
                if (input)
                {
                    const int have = frames ? std::clamp(frames(clock, block), 0, block) : block;
                    for (int i = 0; i < block; ++i)
                        in[(size_t) i] = i < have ? input(clock + i) : 1.0e3f;
                    e->setAudioInput(have > 0 ? AudioInput { in.data(), in.data(), have, 0.5f } : AudioInput {});
                }
                else
                    e->setAudioInput({});
                buf.clear();
                e->processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(buf.getSample(0, i));
                clock += block;
            }
            return out;
        }
    };

    // A sound of each engine with a phase destination that is a sine at its note, and that destination.
    struct PhaseCase { EngineType type; const char* label; std::vector<std::pair<int, float>> sound; };
    static std::vector<PhaseCase> phaseCases()
    {
        std::vector<std::pair<int, float>> fm { { FMEngine::AlgorithmParam, 5.0f }, { FMEngine::MasterLevel, 0.6f } };
        for (int op = 0; op < 6; ++op)
        {
            fm.push_back({ op * 10 + 1, op == 0 ? 0.9f : 0.0f });
            fm.push_back({ op * 10 + 3, 0.001f });      // attack
            fm.push_back({ op * 10 + 5, 1.0f });        // sustain
        }
        return {
            { EngineType::Wavetable, "PM", { { WavetableEngine::TableSelect, 3.0f }, { WavetableEngine::AmpAttack, 0.001f }, { WavetableEngine::AmpSustain, 1.0f } } },
            { EngineType::FM, "PM1", fm },
            { EngineType::WaveSeq, "PM", { { WaveSeqEngine::SeqLength, 1.0f }, { WaveSeqEngine::StepShape, 0.0f }, { WaveSeqEngine::ShapeMix, 0.0f },
                                           { WaveSeqEngine::AmpAttack, 0.001f }, { WaveSeqEngine::AmpSustain, 1.0f } } } };
    }

    // phaseosc::readAt before U4-1 (its general path for every wave), for the comparison.
    static float oldReadAt(const phaseosc::Wave& w, phaseosc::Copy copy, double phase, float position)
    {
        const Wavetable& table = *w.table;
        if (! (phase >= 0.0 && phase < 1.0))
            phase = 0.0;
        float placeA = phaseosc::unit(position), placeB = 0.0f, into = 0.0f;
        if (w.steps >= 2)
        {
            const phaseosc::SequencePlace place = phaseosc::sequencePlace(position, w.steps, w.fade);
            const float last = (float) (w.steps - 1);
            placeA = (float) place.wave / last;
            placeB = (float) (place.wave + 1) / last;
            into = place.into;
        }
        auto at = [&](int level)
        {
            const float a = WavetableRead::sample(table, level, placeA, phase, w.interp);
            if (! (into > 0.0f))
                return a;
            return a + (WavetableRead::sample(table, level, placeB, phase, w.interp) - a) * into;
        };
        const float a = at(copy.level);
        if (! (copy.next > 0.0f))
            return a;
        return a + (at(copy.level + 1) - a) * copy.next;
    }

    void runTest() override
    {
        testPublished();
        testPhase();
        testInputEnds();
        testReadAt();
        testFold();
        testReview();

        beginTest("Nothing else moved: every router engine's factory presets and five sounds at 44.1 / 48 / 96 kHz in blocks of 37 and 100, bit for bit against 46fedbc");
        {
            const auto cases = grid();
            const bool dump = env("NEPTUNE_ROUTER_PHASE_DUMP").isNotEmpty();
            int compared = 0, moved = 0;
           #if JUCE_DEBUG
            const bool debug = true;
           #else
            const bool debug = false;
           #endif
            for (auto& c : cases)
            {
                expect(c.peak > 1.0e-4f && c.peak < 8.0f, c.key + ": sounds, finite (peak " + juce::String(c.peak, 4) + ")");
                if (dump)
                    logMessage("PIN { \"" + c.key + "\", 0x" + juce::String::toHexString((int) c.hash).paddedLeft('0', 8) + "u },");
                const auto pin = pins().find(c.key);
                if (! debug || pin == pins().end())
                    continue;
                ++compared;
                if (pin->second != c.hash)
                {
                    ++moved;
                    logMessage("MOVED: " + c.key);
                }
                expect(pin->second == c.hash, c.key + ": the same samples as before");
            }
            logMessage(juce::String((int) cases.size()) + " renders, " + juce::String(compared) + " compared with their pins, " + juce::String(moved) + " moved");
            if (debug && ! dump)
                expectEquals(compared, (int) cases.size(), "every render has its pin");
        }
    }

    // =============================================================================================
    // (a) An engine's audio-rate source: a constant sub-block late
    void testPublished()
    {
        beginTest("(a) What a voice makes is heard exactly one whole sub-block later at host blocks of 1, 5, 37, 64, 100 and 44.1 / 48 / 96 / 192 kHz; an Immediate source in its own sub-block");
        // how: 0 made, handed over after it is filled (FM); 1 made, the buffer handed over first and
        // filled as the voice renders (the macro oscillator); 2 Immediate, before tick(). (At 192 kHz
        // a sub-block is 64 samples: the whole of the voice's ring.)
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (int block : { 1, 5, 37, 64, 100 })
                for (int how : { 0, 1, 2 })
                {
                    const int which = how == 2 ? (int) Bench::NOW : (int) Bench::MADE;
                    Bench b(rate, block);
                    b.slot(0, mod::Src::Published1 + which, Bench::X, 1.0f);
                    b.v.noteOn(b.r, 60, 1.0f);
                    std::vector<float> heard, made((size_t) R::MAX_CHUNK);
                    const auto fill = [&](long long t, int n)
                    {
                        for (int i = 0; i < n; ++i)
                            made[(size_t) i] = madeAt(t + i);
                    };
                    b.run((long long) (0.03 * rate),
                          [&](long long t, int n) { if (how == 2) { fill(t, n); b.v.publishAudio(which, made.data(), n); } },
                          [&](long long t, int n)
                          {
                              b.collect(Bench::X, n, heard);
                              if (how == 1)
                                  b.v.publishAudio(which, made.data(), n);
                              if (how != 2)
                                  fill(t, n);
                              if (how == 0)
                                  b.v.publishAudio(which, made.data(), n);
                          });
                    const int delay = which == Bench::MADE ? b.r.chunkSize() : 0;
                    int wrong = 0, first = -1;
                    for (size_t t = 0; t < heard.size(); ++t)
                        if (heard[t] != ((int) t < delay ? 0.0f : madeAt((long long) t - delay)))
                        {
                            ++wrong;
                            first = first < 0 ? (int) t : first;
                        }
                    expectEquals(wrong, 0, juce::String(how == 0 ? "made, handed after" : (how == 1 ? "made, handed before it is filled" : "Immediate")) + " at " + juce::String((int) rate) + " Hz in blocks of "
                                 + juce::String(block) + ": the delay is " + juce::String(delay) + " samples (first wrong at " + juce::String(first) + ")");
                }

        beginTest("(a) A source taken back is silence; one handed over again, and a note-on, begin from silence");
        {
            Bench b(48000.0, 64);
            b.slot(0, mod::Src::Published1 + Bench::MADE, Bench::X, 1.0f);
            b.v.noteOn(b.r, 60, 1.0f);
            std::vector<float> heard, made((size_t) R::MAX_CHUNK, 0.5f);
            bool handing = true;
            b.run(4800, [&](long long, int n) { b.collect(Bench::X, n, heard); if (handing) b.v.publishAudio(Bench::MADE, made.data(), n); });
            expect(heard.back() == 0.5f, "heard while handed over");
            b.v.publishAudio(Bench::MADE, nullptr, 0);
            handing = false;
            heard.clear();
            b.run(64, [&](long long, int n) { b.collect(Bench::X, n, heard); });
            expect(heard.front() == 0.0f && heard.back() == 0.0f, "taken back: silence at once");
            expect(! b.v.publishing(Bench::MADE));
            handing = true;
            heard.clear();
            b.run(32, [&](long long, int n) { b.collect(Bench::X, n, heard); if (handing) b.v.publishAudio(Bench::MADE, made.data(), n); });
            expect(heard[0] == 0.0f && heard[15] == 0.0f && heard[16] == 0.5f, "handed over again: a sub-block of silence first");
            b.v.noteOn(b.r, 62, 1.0f);
            heard.clear();
            b.run(32, [&](long long, int n) { b.collect(Bench::X, n, heard); b.v.publishAudio(Bench::MADE, made.data(), n); });
            expect(heard[0] == 0.0f && heard[15] == 0.0f && heard[16] == 0.5f, "a note-on: nothing of the last note");
        }

        beginTest("(a) FM: a slot OP2 > PM1 renders the same samples at every host block (1, 5, 37, 100 against 64) at 44.1 / 48 / 96 kHz");
        {
            std::vector<std::pair<int, float>> sound { { FMEngine::AlgorithmParam, 5.0f }, { FMEngine::MasterLevel, 0.6f } };
            for (int op = 0; op < 6; ++op)
                sound.push_back({ op * 10 + 1, op < 2 ? 0.8f : 0.0f });
            sound.push_back({ 1 * 10 + 0, 2.0f });          // operator 2 an octave up
            for (double rate : { 44100.0, 48000.0, 96000.0 })
            {
                auto render = [&](int block, bool routed)
                {
                    Live l(EngineType::FM, rate, block);
                    l.set(sound);
                    if (routed)
                        l.set({ { ModSlotParam::id(0, ModSlotParam::SRC), (float) sourcePlace(*l.e, "OP2") },
                                { ModSlotParam::id(0, ModSlotParam::DEST), (float) destIndex(*l.e, "PM1") },
                                { ModSlotParam::id(0, ModSlotParam::AMT), 1.0f } });
                    l.e->noteOn(57, 0.9f, -1);
                    auto y = l.run(0.25);
                    y.resize((size_t) (0.24 * rate));
                    return y;
                };
                const auto plain = render(64, false), routed = render(64, true);
                expect(plain != routed, "the slot is heard");
                for (int block : { 1, 5, 37, 100 })
                {
                    expect(render(block, false) == plain, juce::String((int) rate) + " Hz, blocks of " + juce::String(block) + ": the plain sound");
                    expect(render(block, true) == routed, juce::String((int) rate) + " Hz, blocks of " + juce::String(block) + ": OP2 > PM1, the same samples as in blocks of 64");
                }
            }
        }
    }

    // =============================================================================================
    // (b) A phase destination comes in and is let go without a step
    void testPhase()
    {
        beginTest("(b) A phase destination: whole periods of a change are dropped, the rest carried off at half a period in 4 ms (cycles, radians; 44.1 / 48 / 96 kHz)");
        for (double rate : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 37, 64, 100 })
                for (int dest : { (int) Bench::PH, (int) Bench::RAD })
                {
                    const bool cycles = dest == Bench::PH;
                    const double period = cycles ? 1.0 : dsp::TWO_PI_D;
                    // (Never faster than any destination is carried: a whole span, 1, in 2 ms.)
                    const double perSample = std::min(0.5 * period, 2.0) / (0.004 * rate);
                    const float held = cycles ? 0.8f : 4.0f;
                    Bench b(rate, block);
                    b.v.noteOn(b.r, 60, 1.0f);
                    std::vector<float> h;
                    const auto take = [&](long long, int n) { b.collect(dest, n, h); };
                    b.run((long long) (0.02 * rate), take);
                    const size_t arrive = h.size();
                    for (int k = 0; k < (cycles ? 1 : 4); ++k)
                        b.slot(k, mod::Src::One, dest, cycles ? held : 1.0f);
                    b.run((long long) (0.03 * rate), take);
                    const size_t leave = h.size();
                    for (int k = 0; k < 4; ++k)
                        b.slot(k, mod::Src::Off, 0, 0.0f);
                    b.run((long long) (0.03 * rate), take);
                    const juce::String name = juce::String(cycles ? "cycles" : "radians") + " at " + juce::String((int) rate) + " Hz, blocks of " + juce::String(block);
                    // How far the phase travels, a whole period being none: the part of the change that is not whole periods.
                    double travelIn = 0.0, travelOut = 0.0;
                    size_t settled = 0;
                    for (size_t i = arrive; i < h.size(); ++i)
                    {
                        double d = (double) h[i] - (double) h[i - 1];
                        d -= period * std::round(d / period);
                        (i < leave ? travelIn : travelOut) += std::abs(d);
                        if (i < leave && settled == 0 && h[i] == held)
                            settled = i;
                    }
                    const double rest = std::abs(held / period - std::round(held / period)) * period;
                    expect(largestTurn(h, arrive, h.size(), period) <= perSample * 1.001 + 1.0e-6,
                           name + ": the largest turn in a sample " + juce::String(largestTurn(h, arrive, h.size(), period), 6) + " against " + juce::String(perSample, 6));
                    expect(travelIn <= rest + 1.0e-4 && travelOut <= rest + 1.0e-4,
                           name + ": it travels only the part of a period (" + juce::String(travelIn, 4) + " in, " + juce::String(travelOut, 4) + " out, of " + juce::String(rest, 4) + ")");
                    expect(settled > 0 && (double) (settled - arrive) <= rest / perSample + 2 * b.r.chunkSize() + 1,
                           name + ": then exactly the slots' sum, after " + juce::String((int) (settled - arrive)) + " samples");
                    expect(h[leave - 1] == held, name + ": held: " + juce::String(h[leave - 1], 6));
                    expect(h.back() == 0.0f && b.v.buffer(dest) == nullptr, name + ": let go, then not moved at all");
                }

        beginTest("(b) An audio-rate slot held on a phase is not bent while another slot comes and goes; exactly its samples once the carry has gone");
        for (double rate : { 48000.0, 96000.0 })
        {
            Bench b(rate, 37);
            b.slot(0, mod::Src::Published1 + Bench::MADE, Bench::PH, 1.0f);
            b.v.noteOn(b.r, 60, 1.0f);
            std::vector<float> h, made((size_t) R::MAX_CHUNK);
            const auto step = [&](long long t, int n)
            {
                b.collect(Bench::PH, n, h);
                for (int i = 0; i < n; ++i)
                    made[(size_t) i] = madeAt(t + i);
                b.v.publishAudio(Bench::MADE, made.data(), n);
            };
            const int delay = b.r.chunkSize();
            b.run((long long) (0.02 * rate), step);
            const size_t arrive = h.size();
            b.slot(1, mod::Src::One, Bench::PH, 0.8f);
            b.run((long long) (0.02 * rate), step);
            const size_t leave = h.size();
            b.slot(1, mod::Src::Off, 0, 0.0f);
            b.run((long long) (0.02 * rate), step);
            // The control part (what is heard less the audio slot's samples) moves as above; the
            // audio slot's samples are in every sample as they are.
            std::vector<float> control(h.size());
            for (size_t t = 0; t < h.size(); ++t)
                control[t] = h[t] - ((int) t < delay ? 0.0f : madeAt((long long) t - delay));
            const double perSample = 0.5 / (0.004 * rate);
            expect(largestTurn(control, arrive, h.size(), 1.0) <= perSample * 1.001 + 1.0e-5, "the control part turns no faster than half a cycle in 4 ms: "
                   + juce::String(largestTurn(control, arrive, h.size(), 1.0), 6));
            int exact = 0, after = 0;
            for (size_t t = arrive + (size_t) (0.01 * rate); t < leave; ++t, ++after)
                exact += h[t] == 0.8f + madeAt((long long) t - delay) ? 1 : 0;
            expectEquals(exact, after, "held: the sum and the audio slot's samples, exactly");
            int exactEnd = 0, afterEnd = 0;
            for (size_t t = leave + (size_t) (0.01 * rate); t < h.size(); ++t, ++afterEnd)
                exactEnd += h[t] == madeAt((long long) t - delay) ? 1 : 0;
            expectEquals(exactEnd, afterEnd, "let go: the audio slot's samples alone, exactly");
        }

        beginTest("(b) A source (or a via) that a slot on a phase begins to read starts where it stands (an LFO never read: its lead does not run the phase off)");
        for (int shape : { 2, 0 })
            for (bool asVia : { false, true })
            {
                Bench b(48000.0, 64);
                b.r.set(R::lfoId(1, R::Rate), 0.05f);
                b.r.set(R::lfoId(1, R::Shape), (float) shape);
                b.r.set(R::lfoId(1, R::Trigger), 0.0f);
                b.v.noteOn(b.r, 60, 1.0f);
                std::vector<float> h;
                const auto take = [&](long long, int n) { b.collect(Bench::PH, n, h); };
                b.run(2400, take);
                const size_t arrive = h.size();
                if (asVia)
                {
                    b.slot(0, mod::Src::One, Bench::PH, 0.8f);      // ONE x 0.8 through LFO2 taken as 0 .. 1
                    b.r.set(ModSlotParam::id(0, ModSlotParam::VIA), (float) b.r.sourceIndex(mod::Src::Lfo2));
                }
                else
                    b.slot(0, mod::Src::Lfo2, Bench::PH, 0.8f);
                b.run(2400, take);
                const double perSample = 0.5 / (0.004 * 48000.0), lfoMoves = 0.8 * 2.0 * 3.1416 * 0.05 / 48000.0;
                expect(largestTurn(h, arrive, h.size(), 1.0) <= perSample * 1.001 + lfoMoves + 1.0e-6,
                       juce::String(shape == 2 ? "square" : "sine") + (asVia ? " as the VIA" : "") + ": " + juce::String(largestTurn(h, arrive, h.size(), 1.0), 6));
            }

        beginTest("(b) Engines: an LFO aimed at the phase under a held note, let go, and aimed again while it goes, steps no sample beyond 1.7 times the note's own (FM PM1, wavetable PM, wave sequencer PM)");
        {
            // A sine at 220 Hz: its largest step grows with its pitch. Carried at half a period in 4 ms
            // the wave runs at most 125 Hz off (1.57 times); stated before the first run: 1.7.
            struct Run { double rate; int block; };
            for (auto& c : phaseCases())
                for (auto run : { Run { 48000.0, 64 }, Run { 44100.0, 37 }, Run { 96000.0, 100 } })
                {
                    Live l(c.type, run.rate, run.block);
                    l.set(c.sound);
                    const int dest = destIndex(*l.e, c.label);
                    l.set({ { R::lfoId(0, R::Rate), 0.02f }, { R::lfoId(0, R::Shape), 2.0f }, { R::lfoId(0, R::Depth), 0.9f },
                            { R::lfoId(0, R::DestCell), 0.0f } });
                    l.e->noteOn(57, 1.0f, -1);
                    auto y = l.run(0.2);
                    const size_t steadyFrom = y.size() - (size_t) (0.1 * run.rate);
                    const float steady = largestStep(y, steadyFrom, y.size());
                    std::vector<size_t> at;
                    const auto then = [&](std::vector<std::pair<int, float>> cells, double seconds)
                    {
                        at.push_back(y.size());
                        l.set(cells);
                        const auto more = l.run(seconds);
                        y.insert(y.end(), more.begin(), more.end());
                    };
                    then({ { R::lfoId(0, R::DestCell), (float) dest } }, 0.1);      // comes
                    then({ { R::lfoId(0, R::DestCell), 0.0f } }, 0.002);            // goes ...
                    then({ { R::lfoId(0, R::DestCell), (float) dest } }, 0.1);      // ... and comes again while it goes
                    then({ { R::lfoId(0, R::Depth), 0.0f } }, 0.1);             // DPTH to 0
                    const juce::String title = name(c.type) + " " + c.label + " at " + juce::String((int) run.rate) + " Hz, blocks of " + juce::String(run.block);
                    expect(steady > 0.002f, title + ": it sounds");
                    for (size_t k = 0; k < at.size(); ++k)
                    {
                        const float worst = largestStep(y, at[k] - 2, std::min(y.size(), at[k] + (size_t) (0.02 * run.rate)));
                        expect(worst <= 1.7f * steady, title + ", change " + juce::String((int) k + 1) + ": largest step " + juce::String(worst, 4) + " against " + juce::String(steady, 4));
                    }
                }
        }
    }

    // =============================================================================================
    // (c) An input that ends or is shorter than the block
    void testInputEnds()
    {
        beginTest("(c) Every router engine: an input that ends inside a block, is away, and comes back is the same samples as that input faded out and in over 4 ms");
        {
            struct Run { double rate; int block; };
            for (auto type : routerEngines())
                for (auto run : { Run { 48000.0, 64 }, Run { 44100.0, 37 }, Run { 96000.0, 100 } })
                {
                    const double rate = run.rate;
                    const long long ends = (long long) (0.2 * rate) + run.block / 2;            // inside a block
                    const long long back = (long long) (0.3 * rate) / run.block * run.block;    // at a block's start
                    const auto tone = [rate](long long t) { return 0.6f * (float) std::sin(6.283185307179586 * 330.0 * (double) t / rate); };
                    // What the router is to make of it: the last sample held and let go of, then let in.
                    std::vector<float> faded((size_t) (0.45 * rate) + (size_t) run.block);
                    {
                        const float step = (float) (1.0 / (0.004 * rate));
                        float gain = 1.0f, tail = 0.0f, last = 0.0f;
                        for (long long t = 0; t < (long long) faded.size(); ++t)
                        {
                            const bool have = t < ends || t >= back;
                            if (! have && gain != 0.0f) { tail = std::clamp(tail + last * gain, -1.0f, 1.0f); gain = 0.0f; }
                            if (tail != 0.0f) tail = dsp::towards(tail, 0.0f, step);
                            float x = 0.0f;
                            if (have)
                            {
                                x = tone(t);
                                last = x;
                                if (gain != 1.0f) { gain = dsp::towards(gain, 1.0f, step); x *= gain; }
                            }
                            if (tail != 0.0f) x = std::clamp(x + tail, -1.0f, 1.0f);
                            faded[(size_t) t] = x;
                        }
                    }
                    auto render = [&](bool cut)
                    {
                        Live l(type, rate, run.block);
                        l.set({ { R::xinId(R::XinDest1), (float) destIndex(*l.e, "AMP") }, { R::xinId(R::XinAmount1), 0.7f },
                                { R::xinId(R::XinDest2), (float) destIndex(*l.e, "CUT") }, { R::xinId(R::XinAmount2), 0.4f },
                                { R::xinId(R::XinFilter2), 1.0f } });
                        if (cut)
                        {
                            l.input = tone;
                            l.frames = [&](long long t, int n) { return t >= back ? n : (int) std::clamp<long long>(ends - t, 0, n); };
                        }
                        else
                            l.input = [&](long long t) { return faded[(size_t) t]; };
                        l.e->noteOn(48, 0.9f, -1);
                        l.e->noteOn(60, 0.7f, -1);
                        return l.run(0.4);
                    };
                    const auto a = render(true), b = render(false);
                    const juce::String title = name(type) + " at " + juce::String((int) rate) + " Hz, blocks of " + juce::String(run.block);
                    size_t first = a.size();
                    for (size_t i = 0; i < a.size() && first == a.size(); ++i)
                        if (a[i] != b[i])
                            first = i;
                    expect(first == a.size(), title + ": the same samples (first apart at " + juce::String((int) first) + ", the input ends at "
                           + juce::String((int) ends) + ", comes back at " + juce::String((int) back) + ")");
                    double level = 0.0;
                    bool finite = true;
                    for (float x : a) { level += (double) x * x; finite = finite && std::isfinite(x); }
                    expect(finite && level > 1.0e-3, title + ": it sounds");
                }
        }

        beginTest("(c) The router: an input that is there when a slot begins to read it is not let in again (the slot's own gain is what is led)");
        for (int block : { 37, 64 })
        {
            // One router reads the input all along (a slot of a thousandth on the control-rate C),
            // the other reads nothing until the slot on X comes: from there they are the same.
            std::vector<float> heard[2];
            for (int k = 0; k < 2; ++k)
            {
                Bench b(48000.0, block);
                b.input = [](long long t) { return 0.5f * (float) std::sin(0.05 * (double) t); };
                if (k == 1)
                    b.slot(1, mod::Src::Xin, Bench::C, 0.001f);
                b.v.noteOn(b.r, 60, 1.0f);
                b.run(480, [](long long, int) {});
                b.slot(0, mod::Src::Xin, Bench::X, 1.0f);
                b.run(960, [&](long long, int n) { b.collect(Bench::X, n, heard[k]); });
            }
            expect(heard[0] == heard[1] && heard[0][100] != 0.0f, "blocks of " + juce::String(block) + ": the same samples whoever read the input before");
        }

        beginTest("(c) The router: what an ended input gave goes to nothing in 4 ms; it comes back from nothing as fast");
        for (double rate : { 44100.0, 96000.0 })
        {
            Bench b(rate, 37);
            b.r.set(R::xinId(R::XinDest1), (float) Bench::X);
            b.r.set(R::xinId(R::XinAmount1), 1.0f);
            b.input = [](long long) { return 0.5f; };
            const long long ends = (long long) (0.01 * rate) + 11, back = (long long) (0.03 * rate) / 37 * 37;
            b.frames = [&](long long t, int n) { return t >= back ? n : (int) std::clamp<long long>(ends - t, 0, n); };
            b.v.noteOn(b.r, 60, 1.0f);
            std::vector<float> h;
            b.run((long long) (0.05 * rate), [&](long long, int n) { b.collect(Bench::X, n, h); });
            const float step = (float) (1.0 / (0.004 * rate));
            expect(h[(size_t) ends - 1] == 0.5f, "there");
            expect(largestStep(h, 1, h.size()) <= step * 1.001f, "no step larger than a 4 ms fade's: " + juce::String(largestStep(h, 1, h.size()), 6));
            expect(h[(size_t) ends + (size_t) (0.004 * rate) + 1] == 0.0f && h[(size_t) back - 1] == 0.0f, "gone after 4 ms");
            expect(h[(size_t) back] > 0.0f && h[(size_t) back] < 0.01f && h[(size_t) back + (size_t) (0.004 * rate) + 1] == 0.5f, "back from nothing in 4 ms");
        }
    }

    // =============================================================================================
    // (d) phaseosc::readAt
    void testReadAt()
    {
        beginTest("(d) phaseosc::readAt: the same samples as before for every INTP, copy, sequence and argument (also out of range)");
        juce::Random random(20261122);
        const auto& tables = WavetableFactory::builtInTables();
        int compared = 0, apart = 0;
        for (size_t t = 0; t < std::min<size_t>(tables.size(), 6); ++t)
            for (int interp = 0; interp < (int) WavetableInterpMode::Count; ++interp)
                for (int steps : { 0, 1, 5 })
                    for (int k = 0; k < 400; ++k)
                    {
                        phaseosc::Wave w = steps >= 2 ? phaseosc::Wave::sequence(tables[t], random.nextFloat(), steps) : phaseosc::Wave::of(tables[t]);
                        w.interp = (WavetableInterpMode) interp;
                        phaseosc::Copy c;
                        c.level = random.nextInt(std::max(1, tables[t]->getMipLevelCount() - 1));
                        c.next = random.nextBool() ? random.nextFloat() : 0.0f;
                        const double phase = k % 50 == 0 ? (k % 100 == 0 ? 1.0 : -0.25) : random.nextDouble();
                        const float position = k % 40 == 0 ? (k % 80 == 0 ? 1.5f : std::numeric_limits<float>::quiet_NaN()) : random.nextFloat();
                        const float a = phaseosc::readAt(w, c, phase, position), b = oldReadAt(w, c, phase, position);
                        ++compared;
                        apart += std::memcmp(&a, &b, sizeof(float)) != 0 ? 1 : 0;
                    }
        expectEquals(apart, 0, juce::String(compared) + " reads compared");
    }

    // =============================================================================================
    // (f) The wavetable: a silent audio slot on POS does not change how the rest of POS is taken
    void testFold()
    {
        using T = U4WavetableTests;
        beginTest("(f) Wavetable: POS 80, LFO1 > POS 50 and an XIN POS slot whose input is silent: as without the slot (it was LOST against the old engine)");
        {
            T::Verdict none, silent, heard;
            for (int which = 0; which < 3; ++which)
            {
                T::Gesture g;
                g.seconds = 1.0;
                g.notes = { { 57, 0.9f, 0.0, 0.8 } };
                g.input = which == 2 ? T::sine(300.0, 0.7f) : std::function<float(long long)>([](long long) { return 0.0f; });
                const T::Patch sound = T::with(T::with(T::plain(), { { WavetableEngine::Position, 0.8f }, { WavetableEngine::AmpSustain, 1.0f } }),
                                               T::lfo(0, 3.0f, 0.5f, WavetableVoice::LfoPosition));
                T::O o;
                WavetableEngine n, alone;
                T::cells(sound)(o);
                T::cells(sound)(n);
                T::cells(sound)(alone);
                T::X cellsOfOld;
                const float depth = which == 0 ? 0.0f : 0.3f;
                for (auto& x : std::vector<std::pair<int, float>> { { T::X::Mode, 2.0f }, { T::X::Depth, depth } })
                {
                    o.setParameter(x.first, x.second);
                    cellsOfOld.set(n, x.first, x.second);
                }
                const auto oldOne = T::render(o, g), newOne = T::render(n, g);
                const T::Verdict v = T::compare(oldOne, newOne);
                (which == 0 ? none : (which == 1 ? silent : heard)) = v;
                logMessage(juce::String(which == 0 ? "no XIN slot" : (which == 1 ? "XIN POS 30, the input silent" : "XIN POS 30, a 300 Hz input")) + ", old against new: " + v.text());
                if (which == 1)
                {
                    const auto without = T::render(alone, g);
                    float most = 0.0f;
                    for (size_t i = 0; i < without.l.size(); ++i)
                        most = std::max(most, std::abs(without.l[i] - newOne.l[i]));
                    expect(most < 1.0e-4f, "the silent slot changes nothing: " + juce::String(most, 7));
                }
            }
            expect(! silent.lost() && std::abs(silent.envelope - none.envelope) < 0.05, "silent: as the LFO alone (" + silent.text() + ")");
            expect(! heard.lost(), "a heard input: " + heard.text());
        }

        beginTest("(f) Wavetable: the input on POS falls silent under a held note: the LFO's share goes back to stopping at the end, without a jump");
        {
            Live l(EngineType::Wavetable, 48000.0, 1);
            auto& w = dynamic_cast<WavetableEngine&>(*l.e);
            long long silentFrom = (long long) (0.15 * 48000.0);
            l.input = [&](long long t) { return t < silentFrom ? 0.5f * (float) std::sin(6.283185307179586 * 300.0 * (double) t / 48000.0) : 0.0f; };
            l.set({ { WavetableEngine::TableSelect, 4.0f }, { WavetableEngine::Position, 0.9f }, { WavetableEngine::AmpSustain, 1.0f },
                    { R::lfoId(0, R::Rate), 0.02f }, { R::lfoId(0, R::Shape), 2.0f }, { R::lfoId(0, R::Depth), 0.3f },
                    { R::lfoId(0, R::DestCell), (float) WavetableVoice::LfoPosition },
                    { ModSlotParam::id(0, ModSlotParam::SRC), (float) sourcePlace(*l.e, "XIN") },
                    { ModSlotParam::id(0, ModSlotParam::DEST), (float) WavetableVoice::LfoPosition },
                    { ModSlotParam::id(0, ModSlotParam::AMT), 0.002f } });
            l.e->noteOn(57, 1.0f, -1);
            float before = 0.0f, worst = 0.0f, heardAt = 0.0f;
            for (int s = 0; s < (int) (0.25 * 48000.0); ++s)
            {
                l.run(1.0 / 48000.0);
                const float now = w.getVoice(0).getReadPosition();
                if (s > 0)
                    worst = std::max(worst, std::abs(now - before));
                before = now;
                if (s == (int) silentFrom - 1)
                    heardAt = now;
            }
            expectWithinAbsoluteError(heardAt, WavetableVoice::foldPosition(1.2f), 3.0e-3f, "heard: POS + 30 mirrored at the end");
            expectWithinAbsoluteError(before, 1.0f, 1.0e-3f, "silent: POS + 30 stopped at the end");
            expect(worst < 0.02f, "the largest move of the position in a sample: " + juce::String(worst, 4) + " (the glide: 0.007)");
        }
    }

    // =============================================================================================
    // The U4-1 review: what the reviewer's mutations found unwatched
    void testReview()
    {
        beginTest("Review: a phase whose span is negative comes and goes travelling only the part of a period, at half a period in 4 ms");
        for (int block : { 37, 64 })
        {
            const double rate = 48000.0;
            Bench b(rate, block);
            b.v.noteOn(b.r, 60, 1.0f);
            std::vector<float> h;
            const auto take = [&](long long, int n) { b.collect(Bench::NEG, n, h); };
            b.run(960, take);
            const size_t arrive = h.size();
            b.slot(0, mod::Src::One, Bench::NEG, 0.8f);
            b.run(1440, take);
            const size_t leave = h.size();
            b.slot(0, mod::Src::Off, 0, 0.0f);
            b.run(1440, take);
            double travel = 0.0;
            for (size_t i = arrive; i < h.size(); ++i)
            {
                double d = (double) h[i] - (double) h[i - 1];
                d -= std::round(d);
                travel += std::abs(d);
            }
            const double perSample = 0.5 / (0.004 * rate);
            const juce::String name = "blocks of " + juce::String(block);
            expect(h[leave - 1] == -0.8f, name + ": held at -0.8: " + juce::String(h[leave - 1], 6));
            expect(travel <= 0.4 + 1.0e-4, name + ": travels 0.2 in and 0.2 out, not " + juce::String(travel, 4));
            expect(largestTurn(h, arrive, h.size(), 1.0) <= perSample * 1.001 + 1.0e-6, name + ": largest turn " + juce::String(largestTurn(h, arrive, h.size(), 1.0), 6));
            expect(h.back() == 0.0f, name + ": let go");
        }

        beginTest("Review: an input that ends while it is being let in, and comes back while what it gave is let go of: the 4 ms fades worked out sample by sample");
        for (double rate : { 44100.0, 48000.0 })
        {
            const int block = 37;
            const long long A = 37 * 30 + 10, B = 37 * 32, C = B + 20, D = 37 * 34;     // ends; back (the tail still going); ends again (still let in); back
            const auto have = [=](long long t) { return t < A || (t >= B && t < C) || t >= D; };
            const auto tone = [](long long t) { return 0.6f * (float) std::sin(0.031 * (double) t); };
            Bench b(rate, block);
            b.slot(0, mod::Src::Xin, Bench::X, 1.0f);
            b.input = tone;
            b.frames = [&](long long t, int n) { int k = 0; while (k < n && have(t + k)) ++k; return k; };
            b.v.noteOn(b.r, 60, 1.0f);
            std::vector<float> h;
            b.run(2400, [&](long long, int n) { b.collect(Bench::X, n, h); });
            std::vector<float> model(h.size());
            {
                const float step = (float) (1.0 / (0.004 * rate));
                float gain = 1.0f, tail = 0.0f, last = 0.0f;
                for (long long t = 0; t < (long long) model.size(); ++t)
                {
                    const bool there = have(t);
                    if (! there && gain != 0.0f) { tail = std::clamp(tail + last * gain, -1.0f, 1.0f); gain = 0.0f; }
                    if (tail != 0.0f) tail = dsp::towards(tail, 0.0f, step);
                    float x = 0.0f;
                    if (there)
                    {
                        x = tone(t);
                        last = x;
                        if (gain != 1.0f) { gain = dsp::towards(gain, 1.0f, step); x *= gain; }
                    }
                    if (tail != 0.0f) x = std::clamp(x + tail, -1.0f, 1.0f);
                    model[(size_t) t] = x;
                }
            }
            int apart = 0, first = -1;
            for (size_t t = 0; t < h.size(); ++t)
                if (h[t] != model[t]) { ++apart; first = first < 0 ? (int) t : first; }
            expectEquals(apart, 0, juce::String((int) rate) + " Hz: samples apart from the fades (first at " + juce::String(first) + ")");
            expect(std::abs(model[(size_t) C - 1]) > 0.0f && model[(size_t) C] != 0.0f, "(the case: a tail at C)");
        }

        beginTest("Review: an engine that stops reading the input while what it gave is let go of, and reads it again later, reads the input as it stands");
        {
            Bench b(48000.0, 64);
            b.input = [](long long) { return 0.5f; };
            const long long ends = 64 * 10 + 20, back = 64 * 12;
            b.frames = [&](long long t, int n) { return t >= back ? n : (int) std::clamp<long long>(ends - t, 0, n); };
            b.v.noteOn(b.r, 60, 1.0f);
            b.r.readsInput(true);
            float tail = 0.0f;
            b.run(64 * 11, [&](long long, int n) { tail = b.r.inputNow()[n - 1]; });
            b.r.readsInput(false);
            b.run(64 * 30, [](long long, int) {});
            b.r.readsInput(true);
            std::vector<float> now;
            b.run(64, [&](long long, int n) { const float* x = b.r.inputNow(); now.insert(now.end(), x, x + n); });
            expect(tail > 0.1f, "(the case: a tail while it read: " + juce::String(tail, 4) + ")");
            int apart = 0;
            for (float x : now)
                apart += x != 0.5f ? 1 : 0;
            expectEquals(apart, 0, "read again: the input as it stands, nothing of the old tail (first " + juce::String(now.front(), 6) + ")");
        }

        beginTest("Review: the phase destinations of every router engine, by name and period");
        {
            const auto phases = [](const mod::DestTable& t, float period)
            {
                juce::StringArray out;
                for (int d = 1; d < t.size(); ++d)
                    if (t[d].period > 0.0f)
                        out.add(juce::String(t[d].label) + (std::abs(t[d].period - period) < 1.0e-6f && std::abs(t[d].cycle - period / std::abs(t[d].span)) < 1.0e-6f ? "" : "?"));
                return out.joinIntoString(" ");
            };
            expectEquals(phases(FMEngine::lfoDests(), dsp::TWO_PI_F), juce::String("PM1 PM2 PM3 PM4 PM5 PM6 PMAL"));
            expectEquals(phases(WavetableEngine::lfoDests(), 1.0f), juce::String("PM"));
            expectEquals(phases(WaveSeqEngine::lfoDests(), 1.0f), juce::String("PM"));
            expectEquals(phases(SamplerEngine::lfoDests(), 1.0f), juce::String());
            for (int model = 0; model < 8; ++model)
                expectEquals(phases(MacroOscEngine::lfoDests(model), 1.0f), juce::String(), "macro model " + juce::String(model));
        }

        beginTest("Review: FM: OP2 > PM1 taken away and aimed again 100 ms later begins from silence, as a slot aimed there for the first time");
        {
            // (Stated before the first run: the two renders within 1e-4 for 10 ms from the slot's return.)
            std::vector<std::pair<int, float>> sound { { FMEngine::AlgorithmParam, 5.0f }, { FMEngine::MasterLevel, 0.6f } };
            for (int op = 0; op < 6; ++op)
                sound.push_back({ op * 10 + 1, op < 2 ? 0.8f : 0.0f });
            sound.push_back({ 1 * 10 + 0, 2.0f });
            const auto render = [&](bool before)
            {
                Live l(EngineType::FM, 48000.0, 64);
                l.set(sound);
                const float src = (float) sourcePlace(*l.e, "OP2");
                l.set({ { ModSlotParam::id(0, ModSlotParam::DEST), (float) destIndex(*l.e, "PM1") }, { ModSlotParam::id(0, ModSlotParam::AMT), 1.0f },
                        { ModSlotParam::id(0, ModSlotParam::SRC), before ? src : 0.0f } });
                l.e->noteOn(57, 0.9f, -1);
                auto y = l.run(0.1);
                l.set({ { ModSlotParam::id(0, ModSlotParam::SRC), 0.0f } });
                auto more = l.run(0.1);
                y.insert(y.end(), more.begin(), more.end());
                l.set({ { ModSlotParam::id(0, ModSlotParam::SRC), src } });
                more = l.run(0.05);
                y.insert(y.end(), more.begin(), more.end());
                return y;
            };
            const auto a = render(true), b = render(false);
            const size_t from = (size_t) (0.2 * 48000.0) / 64 * 64;
            float most = 0.0f, after = 0.0f;
            for (size_t i = from; i < from + 480; ++i)
                most = std::max(most, std::abs(a[i] - b[i]));
            for (size_t i = from; i < a.size(); ++i)
                after = std::max(after, std::abs(a[i] - b[i]));
            expect(a[0] != b[0] || a[2000] != b[2000], "(the slot was heard before)");
            expect(most < 1.0e-4f, "10 ms from the slot's return: " + juce::String(most, 7));
            logMessage("FM re-tap: largest difference after the return " + juce::String(after, 7));
        }
    }
};

static RouterPhaseTests routerPhaseTests;
