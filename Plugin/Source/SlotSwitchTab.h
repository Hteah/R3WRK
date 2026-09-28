#pragma once
#include <JuceHeader.h>
#include "R3WRKLookAndFeel.h"

/**
    The tiny tab above a drawer pill that flips a shared FX-drawer slot to its other effect
    (RTRG <-> CHO). It names where a click takes you -- "⇄ CHO" on the RTRG side, "⇄ RTRG" on
    the CHO side -- in dim ink, with a border only on hover, so it reads as secondary to the pill
    under it. Colours come from the owning panel's applyTheme().
*/
struct SlotSwitchTab : juce::Component, public juce::SettableTooltipClient
{
    juce::String target;             // the effect a click switches to
    juce::Colour ink, border;
    bool hovered = false;
    std::function<void()> onClick;

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        if (hovered)
        {
            g.setColour(border.withAlpha(0.7f));
            g.drawRoundedRectangle(r, 2.5f, 1.0f);
        }
        g.setColour(hovered ? ink : ink.withAlpha(0.8f));
        g.setFont(systemUIFont(8.5f));
        g.drawFittedText(juce::String::fromUTF8("\xE2\x87\x84 ") + target, getLocalBounds().reduced(1, 0),
                         juce::Justification::centred, 1, 0.75f);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
    }
    void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; repaint(); }
};
