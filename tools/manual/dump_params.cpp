#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SRCROOT/engine/AudioEngine.h"
#include "SRCROOT/dsp/effects/TrackFx.h"
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
    for (int t = 0; t <= (int) EngineType::MIDITrack; ++t)
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
        o->setProperty("specs", a);
        fx.add(juce::var(o));
    }
    root->setProperty("effects", fx);
    juce::Array<juce::var> blocks;
    blocks.add(pageVar(DirtBlock::page())); blocks.add(pageVar(TalkBlock::page())); blocks.add(pageVar(ModFxBlock::page()));
    root->setProperty("blocks", blocks);
    puts(juce::JSON::toString(juce::var(root)).toRawUTF8());
    return 0;
}
