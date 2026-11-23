#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SRCROOT/engine/AudioEngine.h"
#include "SRCROOT/project/PresetManager.h"
#include <cstdio>

static juce::var specVar(const ParamSpec& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("label", juce::String(s.label));
    o->setProperty("id", s.id);
    o->setProperty("min", s.minVal);
    o->setProperty("max", s.maxVal);
    o->setProperty("def", s.defaultVal);
    o->setProperty("kind", (int) s.kind);
    o->setProperty("unit", juce::String(s.unit ? s.unit : ""));
    juce::Array<juce::var> ch;
    if (s.choices) for (int i = 0; i < s.numChoices; ++i) ch.add(juce::String(s.choices[i]));
    o->setProperty("choices", ch);
    return juce::var(o);
}
static juce::var pageVar(const ParamPage& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("name", juce::String(p.name));
    o->setProperty("section", juce::String(p.section));
    juce::Array<juce::var> a;
    for (auto& s : p.params) a.add(specVar(s));
    o->setProperty("specs", a);
    return juce::var(o);
}
int main()
{
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> engines;
    for (int t = 0; t < ENGINE_TYPE_COUNT; ++t)
    {
        auto e = AudioEngine::createEngine((EngineType) t);
        if (!e) continue;
        e->prepareToPlay(48000.0, 64);
        auto* o = new juce::DynamicObject();
        o->setProperty("name", e->getName());
        o->setProperty("type", t);
        juce::Array<juce::var> pages;
        for (auto& p : e->getParamPages()) pages.add(pageVar(p));
        o->setProperty("pages", pages);
        engines.add(juce::var(o));
    }
    root->setProperty("engines", engines);
    juce::Array<juce::var> fx;
    for (int t = 0; t < 64; ++t)
    {
        auto e = AudioEngine::createEffect((EffectType) t);
        if (!e) break;
        auto* o = new juce::DynamicObject();
        o->setProperty("name", juce::String(AudioEngine::effectTypeName((EffectType) t)));
        juce::Array<juce::var> a;
        for (int i = 0; i < 4; ++i) a.add(specVar(e->getParamSpec(i)));
        for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)                    // the cells of page FX2
            if (e->getExtraSpec(i).isValid()) a.add(specVar(e->getExtraSpec(i)));
        o->setProperty("specs", a);
        fx.add(juce::var(o));
    }
    root->setProperty("effects", fx);
    // The built-in presets and multi presets, in the order PRESET lists them (nothing is read from disk).
    PresetManager manager;
    juce::Array<juce::var> presets;
    for (int i = 0; i < manager.getPresetCount(); ++i)
    {
        const auto& p = manager.getPreset(i);
        if (!p.isFactory) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("name", p.name);
        o->setProperty("category", p.category);
        o->setProperty("engine", (int) p.engineType);
        presets.add(juce::var(o));
    }
    root->setProperty("presets", presets);
    juce::Array<juce::var> multis;
    for (int i = 0; i < manager.getMultiCount(); ++i)
    {
        const auto& m = manager.getMulti(i);
        auto* o = new juce::DynamicObject();
        o->setProperty("name", m.name);
        o->setProperty("play", m.playMember);
        juce::Array<juce::var> members;
        for (auto& member : m.members)
        {
            auto* mo = new juce::DynamicObject();
            mo->setProperty("name", member.preset.name);
            mo->setProperty("engine", (int) member.preset.engineType);
            mo->setProperty("pattern", member.hasPattern);
            members.add(juce::var(mo));
        }
        o->setProperty("members", members);
        multis.add(juce::var(o));
    }
    root->setProperty("multis", multis);
    puts(juce::JSON::toString(juce::var(root)).toRawUTF8());
    return 0;
}
