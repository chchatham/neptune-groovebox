#pragma once

#include "../ScreenManager.h"
#include "../NoteUtils.h"
#include "../ParamConstants.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"

class MixerScreen : public Screen
{
public:
    MixerScreen(SequencerCore& seq, AudioEngine* audio)
        : sequencer(seq), audioEngine(audio) {}

    juce::String getName() const override { return "MIXER"; }

    juce::String getSubPage() const override
    {
        int sel = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(sel);
        float vol = getTrackParam(track, PARAM_VOL, 0.8f);
        int pct = (int)(vol * 100.0f);
        return juce::String(pct) + "%";
    }

    void render(DisplayRenderer& display) override
    {
        int trackCount = juce::jmin(8, sequencer.getPattern().getTrackCount());
        int selected = sequencer.getSelectedTrack();

        for (int i = 0; i < trackCount; ++i)
        {
            int x = i * 16;
            auto& track = sequencer.getPattern().getTrack(i);

            // Track number
            display.drawChar(x + 5, 12, '1' + (char)i);

            // Engine type abbreviation
            auto eng = NoteUtils::engineAbbrev((int)track.engineType);
            display.drawString(x + 1, 20, eng);

            // Mute / Solo indicator
            if (track.muted)
                display.drawString(x + 5, 28, "M");
            else if (track.soloed)
                display.drawString(x + 5, 28, "S");

            // Level bar (outline + fill)
            float vol = getTrackParam(track, PARAM_VOL, 0.8f);
            int barH = 12;
            display.drawRect(x + 2, 34, 12, barH, false);
            int fillH = (int)(vol * (float)(barH - 2));
            if (fillH > 0)
            {
                int fillY = 34 + barH - 1 - fillH;
                display.drawRect(x + 3, fillY, 10, fillH, true);
            }

            // Pan indicator: dot at position within column
            // pan: -1 (left) to +1 (right), center = 0
            float pan = getTrackParam(track, PARAM_PAN, 0.0f);
            int panX = x + 2 + (int)((pan + 1.0f) * 0.5f * 10.0f);
            panX = juce::jlimit(x + 2, x + 12, panX);
            display.setPixel(panX, 48, true);
            display.setPixel(panX + 1, 48, true);
            // Center tick mark
            display.setPixel(x + 7, 47, true);

            // Send indicators: small dots when send levels are non-zero
            float snd1 = getTrackParam(track, PARAM_SND1, 0.0f);
            float snd2 = getTrackParam(track, PARAM_SND2, 0.0f);
            if (snd1 > 0.01f)
            {
                display.setPixel(x + 3, 51, true);
                display.setPixel(x + 4, 51, true);
            }
            if (snd2 > 0.01f)
            {
                display.setPixel(x + 10, 51, true);
                display.setPixel(x + 11, 51, true);
            }
        }

        // Selection highlight via invertRegion
        if (selected >= 0 && selected < trackCount)
            display.invertRegion(selected * 16, 11, 16, 42);
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (encoderIndex >= 0 && encoderIndex < 8)
            return "C" + juce::String(encoderIndex + 1);
        return {};
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        if (encoderIndex < 0 || encoderIndex >= 8) return;

        int delta = (value > 64) ? -(128 - value) : value;
        auto& track = sequencer.getPattern().getTrack(encoderIndex);
        float vol = getTrackParam(track, PARAM_VOL, 0.8f);
        vol = juce::jlimit(0.0f, 1.0f, vol + delta * 0.02f);
        track.soundParams[PARAM_VOL] = vol;

        if (audioEngine)
        {
            auto* engine = audioEngine->getEngine(encoderIndex);
            if (engine)
                engine->setParameter(PARAM_VOL, vol);
        }

        selectedChannel = encoderIndex;
    }

    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        if (padIndex >= 0 && padIndex < sequencer.getPattern().getTrackCount())
        {
            sequencer.setSelectedTrack(padIndex);
            selectedChannel = padIndex;
        }
    }

private:
    static constexpr int PARAM_VOL  = ParamConstants::PARAM_VOL;
    static constexpr int PARAM_PAN  = ParamConstants::PARAM_PAN;
    static constexpr int PARAM_SND1 = ParamConstants::PARAM_SND1;
    static constexpr int PARAM_SND2 = ParamConstants::PARAM_SND2;

    static float getTrackParam(const Track& track, int paramId, float defaultVal)
    {
        return ParamConstants::getTrackParam(track, paramId, defaultVal);
    }

    SequencerCore& sequencer;
    AudioEngine* audioEngine;
    int selectedChannel = 0;
};
