#pragma once
#include <JuceHeader.h>
#include "R3WRKLookAndFeel.h"

/**
    The tiny tab above a drawer pill that flips a shared FX-drawer slot to its other effect
    (RTRG <-> CHO). It names where a click takes you -- "⇄ CHO" on the RTRG side, "⇄ RTRG" on
    the CHO side -- in dim ink (a little brighter on hover, no box), so it reads as secondary to
    the toggle under it. Colours come from the owning panel's applyTheme(). `justification`: the
    FX drawer left-aligns it with its toggle's text (FxRow::packSlot) so every slot's tab sits
    the same way whatever the two names' widths.
*/
struct SlotSwitchTab : juce::Component, public juce::SettableTooltipClient
{
    juce::String target;             // the effect a click switches to
    juce::Colour ink, border;
    bool hovered = false;
    juce::Justification justification { juce::Justification::centred };
    std::function<void()> onClick;

    void paint(juce::Graphics& g) override
    {
        g.setColour(hovered ? ink : ink.withAlpha(0.8f));
        g.setFont(systemUIFont(8.5f));
        g.drawFittedText(juce::String::fromUTF8("\xE2\x87\x84 ") + target,
                         justification == juce::Justification::centred ? getLocalBounds().reduced(1, 0) : getLocalBounds(),
                         justification, 1, 0.75f);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
    }
    void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; repaint(); }
};
