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
        virtualController.setDisplay(&display);     // the panel's small display window shows the live picture

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
    juce::TooltipWindow tooltips { this, 500 };     // the panel's caps carry no names: the pointer says what a control is

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};
