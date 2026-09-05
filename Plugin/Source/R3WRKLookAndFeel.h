#pragma once
#include <JuceHeader.h>
#include "Theme.h"

/**
    The shared custom look: rotary knobs (a flat disc, a thin outline, a single pointer
    line -- no value-arc, Eurorack/VCV-module-inspired) and pill-shaped buttons (fully
    rounded, filled when "on" or accent-primary, outlined/transparent otherwise).
    Reads live from the shared theme, so it stays in sync with theme changes the same way
    the rest of the UI does.

    Any component that wants this look calls setLookAndFeel(&lnf) on itself/its children
    with its own instance of this class (stateless beyond the theme pointer, so multiple
    instances render identically) and must setLookAndFeel(nullptr) on them again before
    its instance is destroyed.
*/
class R3WRKLookAndFeel : public juce::LookAndFeel_V4
{
public:
    R3WRKLookAndFeel() = default;

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    juce::SharedResourcePointer<ThemeManager> theme;
};
