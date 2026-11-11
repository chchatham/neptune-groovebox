#pragma once

#include "../ScreenManager.h"
#include "../../controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>
#include <array>

// SubParamOverlay: latched Shift+turn popup for sub-parameter editing.
//
// Behavior:
//   - When Shift is held and an encoder is turned, this overlay appears
//     with the sub-parameter name and a value bar.
//   - Pitch/mod strips override to sub-param control while overlay is visible.
//   - Dismissed with a lone Shift tap (press+release without turning).
//   - Renders on top of current screen content (centered popup).
//
// Default sub-param mappings per page:
//   SRC:  osc fine tune, waveshape, sync on/off, wave stacking mode
//   FLTR: filter model, key track amount, env amount, env velocity
//   AMP:  velocity sensitivity, pan law, amp curve, output level
//   FX:   send bus type, insert slot select, effect bypass, dry/wet
//   MOD:  LFO shape, LFO sync, env curve, mod source select
//
// Audio-thread safe: no allocations during render. All state is
// pre-allocated and sized at compile time.

struct SubParamMapping
{
    const char* pageName;       // which page this applies to (e.g., "SRC")
    struct Entry
    {
        const char* name;       // display name (up to 12 chars)
        int paramId;            // ParamId for sub-parameter
        float minVal;
        float maxVal;
        float defaultVal;
        bool isDiscrete;        // if true, step in integers
    };
    std::array<Entry, 8> entries;  // one per encoder
};

class SubParamOverlay
{
public:
    // Default sub-parameter mappings for all pages.
    // ParamId range 300+ reserved for sub-parameters.
    static constexpr int SUB_PARAM_BASE = 300;

    // SRC page sub-params (encoders 0-7)
    static constexpr int SP_OSC_FINE_TUNE    = 300;
    static constexpr int SP_OSC_WAVESHAPE    = 301;
    static constexpr int SP_OSC_SYNC         = 302;
    static constexpr int SP_WAVE_STACK_MODE  = 303;
    static constexpr int SP_OSC2_FINE_TUNE   = 304;
    static constexpr int SP_OSC2_WAVESHAPE   = 305;
    static constexpr int SP_SUB_SHAPE        = 306;
    static constexpr int SP_GLIDE_CURVE      = 307;

    // FLTR page sub-params
    static constexpr int SP_FILTER_MODEL     = 310;
    static constexpr int SP_KEY_TRACK        = 311;
    static constexpr int SP_ENV_AMOUNT       = 312;
    static constexpr int SP_ENV_VELOCITY     = 313;
    static constexpr int SP_FILTER_SLOPE     = 314;
    static constexpr int SP_FILTER_DRIVE     = 315;
    static constexpr int SP_MORPH_CURVE      = 316;
    static constexpr int SP_FILTER_PAN       = 317;

    // AMP page sub-params
    static constexpr int SP_VEL_SENSITIVITY  = 320;
    static constexpr int SP_PAN_LAW          = 321;
    static constexpr int SP_AMP_CURVE        = 322;
    static constexpr int SP_OUTPUT_LEVEL     = 323;
    static constexpr int SP_ATK_CURVE        = 324;
    static constexpr int SP_DEC_CURVE        = 325;
    static constexpr int SP_SUS_CURVE        = 326;
    static constexpr int SP_REL_CURVE        = 327;

    // FX page sub-params
    static constexpr int SP_SEND_BUS_TYPE    = 330;
    static constexpr int SP_INSERT_SLOT      = 331;
    static constexpr int SP_EFFECT_BYPASS    = 332;
    static constexpr int SP_DRY_WET          = 333;
    static constexpr int SP_FX_P1_FINE       = 334;
    static constexpr int SP_FX_P2_FINE       = 335;
    static constexpr int SP_FX_P3_FINE       = 336;
    static constexpr int SP_FX_P4_FINE       = 337;

    // MOD page sub-params
    static constexpr int SP_LFO1_SHAPE       = 340;
    static constexpr int SP_LFO1_SYNC        = 341;
    static constexpr int SP_LFO2_SHAPE       = 342;
    static constexpr int SP_LFO2_SYNC        = 343;
    static constexpr int SP_ENV_CURVE        = 344;
    static constexpr int SP_MOD_SOURCE_SEL   = 345;
    static constexpr int SP_ENV_CHAIN_MODE   = 346;
    static constexpr int SP_MOD_DEPTH        = 347;

    SubParamOverlay() { initDefaultMappings(); }

    // Look up a sub-param entry for a given page name and encoder index.
    // Returns nullptr if no sub-param is defined.
    const SubParamMapping::Entry* getEntry(const juce::String& pageName, int encoderIndex) const
    {
        if (encoderIndex < 0 || encoderIndex >= 8)
            return nullptr;

        for (int i = 0; i < mappingCount; ++i)
        {
            if (juce::String(mappings[i].pageName) == pageName)
            {
                auto& e = mappings[i].entries[encoderIndex];
                if (e.name != nullptr && e.name[0] != '\0')
                    return &e;
                return nullptr;
            }
        }
        return nullptr;
    }

    // State management
    bool isActive() const { return active; }
    int getActiveEncoder() const { return activeEncoder; }
    juce::String getActiveName() const { return activeName; }
    float getNormalizedValue() const { return normalizedValue; }

    void activate(int encoder, const char* name, float normValue)
    {
        active = true;
        activeEncoder = encoder;
        activeName = juce::String(name);
        normalizedValue = normValue;
    }

    void updateValue(float normValue)
    {
        normalizedValue = std::clamp(normValue, 0.0f, 1.0f);
    }

    void dismiss()
    {
        active = false;
        activeEncoder = -1;
        activeName = {};
        normalizedValue = 0.0f;
    }

    // Render the overlay popup onto the display, on top of existing content.
    // Call after the screen has rendered its normal content.
    void render(DisplayRenderer& display) const
    {
        if (!active)
            return;

        constexpr int popupW = 80;
        constexpr int popupH = 22;
        int popupX = (DisplayRenderer::WIDTH - popupW) / 2;
        int popupY = (DisplayRenderer::HEIGHT - popupH) / 2;

        // Clear popup area
        for (int y = popupY; y < popupY + popupH; ++y)
            for (int x = popupX; x < popupX + popupW; ++x)
                display.setPixel(x, y, false);

        // Border
        display.drawRect(popupX, popupY, popupW, popupH, false);

        // Name text
        juce::String displayName = activeName.isNotEmpty() ? activeName : juce::String("SUB");
        display.drawString(popupX + 2, popupY + 3, displayName);

        // Value bar
        int barX = popupX + 2;
        int barY = popupY + 13;
        int barW = popupW - 4;
        int barH = 5;
        display.drawRect(barX, barY, barW, barH, false);

        int fillW = (int)(normalizedValue * (float)(barW - 2));
        if (fillW > 0)
            display.drawRect(barX + 1, barY + 1, fillW, barH - 2, true);
    }

    // Return a formatted display string for a sub-param value
    static juce::String formatSubParamValue(float value, const SubParamMapping::Entry& entry)
    {
        if (entry.isDiscrete)
            return juce::String((int)value);

        float absVal = std::abs(value);
        if (absVal >= 100.0f)
            return juce::String((int)value);
        if (absVal >= 10.0f)
            return juce::String(value, 1).substring(0, 4);
        return juce::String(value, 2).substring(0, 4);
    }

    // Strip override: when overlay is active, pitch/mod strips control sub-param
    bool shouldOverrideStrip() const { return active; }

    // Compute a delta value from strip position for sub-param control
    static float stripDelta(int stripValue, float minVal, float maxVal)
    {
        float range = maxVal - minVal;
        return (stripValue / 127.0f) * range;
    }

private:
    void initDefaultMappings()
    {
        // SRC page
        mappings[0] = {"SRC", {{
            {"FineTune",  SP_OSC_FINE_TUNE,   -1.0f,  1.0f,  0.0f, false},
            {"WaveShp",   SP_OSC_WAVESHAPE,    0.0f,  1.0f,  0.0f, false},
            {"Sync",      SP_OSC_SYNC,         0.0f,  1.0f,  0.0f, true},
            {"WaveStk",   SP_WAVE_STACK_MODE,  0.0f,  3.0f,  0.0f, true},
            {"O2Fine",    SP_OSC2_FINE_TUNE,  -1.0f,  1.0f,  0.0f, false},
            {"O2WvShp",   SP_OSC2_WAVESHAPE,   0.0f,  1.0f,  0.0f, false},
            {"SubShp",    SP_SUB_SHAPE,        0.0f,  3.0f,  0.0f, true},
            {"GldCurv",   SP_GLIDE_CURVE,     -1.0f,  1.0f,  0.0f, false}
        }}};

        // FLTR page
        mappings[1] = {"FLTR", {{
            {"FltModel",  SP_FILTER_MODEL,     0.0f,  3.0f,  0.0f, true},
            {"KeyTrk",    SP_KEY_TRACK,        0.0f,  1.0f,  0.5f, false},
            {"EnvAmt",    SP_ENV_AMOUNT,       -1.0f, 1.0f,  0.0f, false},
            {"EnvVel",    SP_ENV_VELOCITY,      0.0f, 1.0f,  0.5f, false},
            {"Slope",     SP_FILTER_SLOPE,      0.0f, 1.0f,  0.0f, true},
            {"FltDrv",    SP_FILTER_DRIVE,      0.0f, 1.0f,  0.0f, false},
            {"MrphCrv",   SP_MORPH_CURVE,      -1.0f, 1.0f,  0.0f, false},
            {"FltPan",    SP_FILTER_PAN,       -1.0f, 1.0f,  0.0f, false}
        }}};

        // AMP page
        mappings[2] = {"AMP", {{
            {"AtkCurv",   SP_ATK_CURVE,       -1.0f,  1.0f,  0.0f, false},
            {"DecCurv",   SP_DEC_CURVE,        -1.0f, 1.0f,  0.0f, false},
            {"SusCurv",   SP_SUS_CURVE,        -1.0f, 1.0f,  0.0f, false},
            {"RelCurv",   SP_REL_CURVE,        -1.0f, 1.0f,  0.0f, false},
            {"OutLvl",    SP_OUTPUT_LEVEL,      0.0f, 1.0f,  1.0f, false},
            {"PanLaw",    SP_PAN_LAW,           0.0f, 2.0f,  1.0f, true},
            {"VelSns",    SP_VEL_SENSITIVITY,   0.0f, 1.0f,  0.5f, false},
            {"AmpCrv",    SP_AMP_CURVE,        -1.0f, 1.0f,  0.0f, false}
        }}};

        // FX page
        mappings[3] = {"FX", {{
            {"BusType",   SP_SEND_BUS_TYPE,    0.0f, 10.0f,  7.0f, true},
            {"InsSlt",    SP_INSERT_SLOT,       0.0f,  7.0f,  0.0f, true},
            {"Bypass",    SP_EFFECT_BYPASS,     0.0f,  1.0f,  0.0f, true},
            {"DryWet",    SP_DRY_WET,           0.0f,  1.0f,  1.0f, false},
            {"P1Fine",    SP_FX_P1_FINE,        0.0f,  1.0f,  0.5f, false},
            {"P2Fine",    SP_FX_P2_FINE,        0.0f,  1.0f,  0.5f, false},
            {"P3Fine",    SP_FX_P3_FINE,        0.0f,  1.0f,  0.5f, false},
            {"P4Fine",    SP_FX_P4_FINE,        0.0f,  1.0f,  0.5f, false}
        }}};

        // MOD page — dynamic layout; sub-params apply to whichever source is selected
        mappings[4] = {"MOD", {{
            {"",          0,                    0.0f,  0.0f,  0.0f, false},  // E0: source select (no sub-param)
            {"",          0,                    0.0f,  0.0f,  0.0f, false},  // E1: count (no sub-param)
            {"",          0,                    0.0f,  0.0f,  0.0f, false},  // E2-E7: dynamic source params
            {"",          0,                    0.0f,  0.0f,  0.0f, false},
            {"",          0,                    0.0f,  0.0f,  0.0f, false},
            {"ModSrc",    SP_MOD_SOURCE_SEL,    0.0f,  7.0f,  0.0f, true},  // E5: mod matrix source select
            {"ChnMode",   SP_ENV_CHAIN_MODE,    0.0f,  2.0f,  0.0f, true},  // E6: chain mode (when ENV selected)
            {"ModDpth",   SP_MOD_DEPTH,         0.0f,  1.0f,  1.0f, false}  // E7: mod depth
        }}};

        mappingCount = 5;
    }

    static constexpr int MAX_MAPPINGS = 8;
    std::array<SubParamMapping, MAX_MAPPINGS> mappings{};
    int mappingCount = 0;

    // Overlay state
    bool active = false;
    int activeEncoder = -1;
    juce::String activeName;
    float normalizedValue = 0.0f;
};
