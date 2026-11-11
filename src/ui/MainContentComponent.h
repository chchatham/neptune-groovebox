#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OLEDMirrorComponent.h"
#include "VirtualControllerComponent.h"

class MainContentComponent : public juce::Component
{
public:
    explicit MainContentComponent(DisplayRenderer& display)
        : oledMirror(display)
    {
        addAndMakeVisible(oledMirror);
        addAndMakeVisible(virtualController);

        int totalW = juce::jmax(OLEDMirrorComponent::TOTAL_W, VirtualControllerComponent::CTRL_W);
        int totalH = OLEDMirrorComponent::TOTAL_H + VirtualControllerComponent::CTRL_H;
        setSize(totalW, totalH);
    }

    void resized() override
    {
        int oledX = (getWidth() - OLEDMirrorComponent::TOTAL_W) / 2;
        oledMirror.setBounds(oledX, 0,
                             OLEDMirrorComponent::TOTAL_W, OLEDMirrorComponent::TOTAL_H);

        int ctrlX = (getWidth() - VirtualControllerComponent::CTRL_W) / 2;
        virtualController.setBounds(ctrlX, OLEDMirrorComponent::TOTAL_H,
                                    VirtualControllerComponent::CTRL_W,
                                    VirtualControllerComponent::CTRL_H);
    }

    OLEDMirrorComponent& getOLEDMirror() { return oledMirror; }
    VirtualControllerComponent& getVirtualController() { return virtualController; }

private:
    OLEDMirrorComponent oledMirror;
    VirtualControllerComponent virtualController;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};
