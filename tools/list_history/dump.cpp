// Prints, for the source tree it is compiled against, every cell of every engine's pages (with the
// choices of a Choice cell) and the unified voice's LFO / envelope destination lists.
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "engine/AudioEngine.h"
#include "engine/UnifiedVoice.h"
#include "dsp/LFO.h"
#include "dsp/filters/FilterSlot.h"
#include <cstdio>

static void list(const char* name, const char* const* labels, int n)
{
    std::printf("LIST\t%s\t%d\t", name, n);
    for (int i = 0; i < n; ++i)
        std::printf("%s%s", i ? "|" : "", labels[i] != nullptr ? labels[i] : "(null)");
    std::printf("\n");
}

int main()
{
    for (int e = 0; e < 16; ++e)
    {
        auto engine = AudioEngine::createEngine((EngineType) e);
        if (engine == nullptr || (int) engine->getType() != e)
            continue;
        const auto pages = engine->getParamPages();
        int pageIndex = 0;
        for (auto& page : pages)
        {
            for (auto& p : page.params)
            {
                if (! p.isValid())
                    continue;
                std::printf("CELL\t%d\t%d\t%s\t%d\t%s\t%d\t%g\t%g\t%d\t", e, pageIndex, page.name.toRawUTF8(), p.id, p.label,
                            (int) p.kind, (double) p.minVal, (double) p.maxVal, p.kind == ParamKind::Choice ? p.numChoices : 0);
                if (p.kind == ParamKind::Choice && p.choices != nullptr)
                    for (int i = 0; i < p.numChoices; ++i)
                        std::printf("%s%s", i ? "|" : "", p.choices[i] != nullptr ? p.choices[i] : "(null)");
                std::printf("\n");
            }
            ++pageIndex;
        }
    }
#ifdef DUMP_UV_CELL_LIST
    list("uv.lfo", UnifiedVoice::lfoDestCellLabels(), UnifiedVoice::NUM_LFO_CELL_DESTS);
#else
    list("uv.lfo", UnifiedVoice::lfoDestLabels(), (int) UnifiedVoice::LfoDest::Count);
#endif
    list("uv.env", UnifiedVoice::envDestLabels(), UnifiedVoice::NUM_ENV_DESTS);
    list("lfo.shape", LFO::shapeLabels(), (int) LFO::Shape::COUNT);
    list("lfo.division", LFO::divisionLabels(), LFO::NUM_DIVISIONS);
    list("lfo.trigger", LFO::triggerLabels(), (int) LFO::TriggerMode::COUNT);
    list("filter.model", FilterSlot::modelNames(), FilterSlot::NUM_MODELS);
    return 0;
}
