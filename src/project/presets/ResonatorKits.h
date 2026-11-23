#pragma once

#include "../Preset.h"
#include "../CymbalPresets.h"
#include "../../engine/DrumEngine.h"
#include <vector>

// Four kits that show what the resonator drum is for (phase U2b of .ralph/spec-unify.md). A kit
// is a preset of the drum engine: its pads, and the kit's own LFOs and mod slots, which every
// resonator drum of the kit hears with its own voice.
// (PresetManager registers them beside FactoryPresets::getAll(), whose own count several suites
// pin; the "Resonator Drum" suite loads and plays them.)
inline std::vector<Preset> resonatorKits()
{
    using D = DrumEngine;
    namespace C = CymbalPresets;
    // (Destinations, sources and tempo divisions by name: a name that is not in its list is
    // remembered - stored::unknownNames, Preset.h - and the preset suites fail on it. A DEST and
    // a SRC begin with OFF, which is no name to route by: from 1.)
    auto dest = [](const char* label) { return stored::placeOf(D::lfoDests().labels(), D::lfoDests().visible(), label, "drum kit destination", 1); };
    auto src = [](const char* label)
    {
        const auto& sources = mod::SourceTable::standard(2);
        return stored::placeOf(sources.labels(), sources.size(), label, "drum kit source", 1);
    };
    auto division = [](const char* label) { return stored::placeOf(LFO::divisionLabels(), LFO::NUM_DIVISIONS, label, "tempo division"); };
    auto lfo = [](int n, int offset) { return EngineLfoSection::BASE + n * EngineLfoSection::PER_LFO + offset; };
    auto slot = [](int n, int offset) { return ModSlotParam::id(n, offset); };
    auto cell = [](int pad, int n) { return D::SOUND_BASE + pad * D::SOUND_CELLS + n; };
    auto make = [](const char* name, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = "KIT";
        p.engineType = EngineType::Drum;
        p.soundParams = std::move(params);
        p.isFactory = true;
        return p;
    };
    const float bodies = (float) (res::NumBodyPoints - 1);
    std::vector<Preset> out;

    // 1. Toms that turn into gongs. Seven heads from low to high, each a point further along MRPH
    //    than its sound (the pad's MRPH + 1: GONG), and the kit's first slot takes that point back
    //    while the stroke's envelope is up: struck as a tom, it opens into a gong as the envelope
    //    falls (1.2 s), and rings on as one.
    {
        ParamMap p;
        const int pads[7] = { 9, 10, 11, 4, 5, 6, 7 };
        const float tunes[7] = { 0.6f, 0.75f, 0.9f, 1.0f, 1.2f, 1.5f, 1.8f };
        for (int i = 0; i < 7; ++i)
        {
            C::resonator(p, pads[i], "TMGN", 0.7f, 0.25f + 0.08f * (float) i, tunes[i]);
            p[cell(pads[i], D::RsnMorph)] = 1.0f;
        }
        p[slot(0, ModSlotParam::SRC)] = src("ENV");
        p[slot(0, ModSlotParam::DEST)] = dest("MRPH");
        p[slot(0, ModSlotParam::AMT)] = -1.0f / bodies;
        out.push_back(make("Tom To Gong Kit", std::move(p)));
    }

    // 2. Brushes. A brushed snare, a brushed ride and a brushed gong: the tap is the velocity, the
    //    swish the pad's pressure, and the kit's two LFOs stir it - LFO 1 on the energy once a
    //    beat, LFO 2 on the hardness once in two (both run freely with the tempo: the hand goes
    //    round whether a pad is struck or not).
    {
        ParamMap p;
        C::resonator(p, 12, "SNRS", 0.8f, 0.5f);
        C::resonator(p, 13, "SNRS", 0.8f, 0.6f, 1.3f, 0.7f);
        C::resonator(p, 7, "RIDS", 0.7f, 0.65f);
        C::resonator(p, 6, "GNGS", 0.7f, 0.35f);
        C::resonator(p, 11, "TOM", 0.7f, 0.4f, 0.9f, 1.2f, 0.4f, 0.0f, 0.3f);      // a soft tom under a mallet
        C::resonator(p, 14, "HATC", 0.6f, 0.6f);
        C::resonator(p, 15, "HATO", 0.6f, 0.6f);
        p[lfo(0, EngineLfoSection::Depth)] = 0.45f;  p[lfo(0, EngineLfoSection::Sync)] = 1.0f;
        p[lfo(0, EngineLfoSection::Division)] = division("1/4");
        p[lfo(0, EngineLfoSection::Trigger)] = (float) (int) LFO::TriggerMode::Free;
        p[lfo(0, EngineLfoSection::Dest)] = dest("ENRG");
        p[lfo(1, EngineLfoSection::Depth)] = 0.3f;   p[lfo(1, EngineLfoSection::Sync)] = 1.0f;
        p[lfo(1, EngineLfoSection::Division)] = division("1/2");
        p[lfo(1, EngineLfoSection::Trigger)] = (float) (int) LFO::TriggerMode::Free;
        p[lfo(1, EngineLfoSection::Dest)] = dest("HARD");
        out.push_back(make("Brush Kit", std::move(p)));
    }

    // 3. One body, every way of playing it: the physical hats (closed and open on the two pads
    //    that cut each other off, half open and the pedal beside them; a finger on the open one
    //    closes it), tom, snare, clave and rim shot, ride and its cup, crash, the gong whose
    //    partials beat, and under the bow (the pad's pressure) a snare, a cymbal, a bar and a bell.
    {
        ParamMap p;
        C::resonator(p, 9, "TOM", 0.75f, 0.4f, 0.8f);
        C::resonator(p, 10, "TOM", 0.75f, 0.6f, 1.3f);
        C::resonator(p, 11, "CLAV", 0.65f, 0.55f);
        C::resonator(p, 12, "SNAR", 0.8f, 0.5f);
        C::resonator(p, 13, "RIM", 0.7f, 0.45f);
        C::resonator(p, 14, "HATC", 0.6f, 0.6f);
        C::resonator(p, 15, "HATO", 0.6f, 0.6f);
        C::resonator(p, 0, "HATH", 0.6f, 0.6f);
        C::resonator(p, 1, "HATP", 0.6f, 0.6f);
        C::resonator(p, 2, "SNRB", 0.8f, 0.5f);
        C::resonator(p, 3, "CYMB", 0.7f, 0.4f);
        C::resonator(p, 4, "BARB", 0.7f, 0.45f);
        C::resonator(p, 5, "BELB", 0.7f, 0.55f);
        C::resonator(p, 6, "GNGB", 0.7f, 0.5f);
        C::resonator(p, 7, "RIDE", 0.6f, 0.65f);
        out.push_back(make("Resonator Kit", std::move(p)));
    }

    // 4. Claves that are bells on some steps. A clave is a short bar: the kit's LFO 1 runs the
    //    custom shape once a bar, stepped (eight steps of an eighth note), on MRPH - where the
    //    shape stands at 0 the clave is the bar it is, at a half it is half-way to a bell, at 1 a
    //    bell. (Exact steps are a step's own lock on the pad's MRPH.)
    {
        ParamMap p;
        const int pads[4] = { 11, 12, 13, 10 };
        const float tunes[4] = { 1.0f, 1.26f, 1.5f, 0.75f };
        for (int i = 0; i < 4; ++i)
            C::resonator(p, pads[i], "CLVB", 0.7f, 0.35f + 0.1f * (float) i, tunes[i]);
        const float shape[LfoShapeParam::POINTS] = { 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
        for (int i = 0; i < LfoShapeParam::POINTS; ++i)
            p[LfoShapeParam::BASE + i] = shape[i];
        p[lfo(0, EngineLfoSection::Shape)] = (float) (int) LFO::Shape::User;
        p[lfo(0, EngineLfoSection::Depth)] = 1.0f / bodies;                         // one point: BAR to BELL
        p[lfo(0, EngineLfoSection::Sync)] = 1.0f;
        p[lfo(0, EngineLfoSection::Division)] = division("1BAR");
        p[lfo(0, EngineLfoSection::Trigger)] = (float) (int) LFO::TriggerMode::Free;
        p[lfo(0, EngineLfoSection::Dest)] = dest("MRPH");
        out.push_back(make("Clave Bells Kit", std::move(p)));
    }
    return out;
}
