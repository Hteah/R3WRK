#pragma once
#include <JuceHeader.h>
#include "Theme.h"

/**
    A flat "hardware panel" rotary knob: a dark disc, a thin outline, and a single pointer
    line showing position -- no value-arc. Modelled on Eurorack/VCV-module-style knobs.
    Reads live from the shared theme (disc/outline/pointer derive from the palette), so it
    stays in sync with theme changes the same way the rest of the UI does.

    Only drawRotarySlider() is overridden -- everything else (buttons, text boxes, ...)
    keeps the default JUCE look for now.
*/
class KnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KnobLookAndFeel() = default;

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<ThemeManager> theme;
};
