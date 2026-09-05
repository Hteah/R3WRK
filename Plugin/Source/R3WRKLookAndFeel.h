#pragma once
#include <JuceHeader.h>
#include "Theme.h"

/**
    The shared custom look: rotary knobs (a flat disc, a thin outline, a single pointer
    line -- no value-arc, Eurorack/VCV-module-inspired), pill-shaped buttons (fully
    rounded, filled when "on" or accent-primary, outlined/transparent otherwise), and a
    handful of drawn transport icons in place of button text (play triangle, stop square,
    loop's two-arrow circle, play-from-start's bar+triangle, Tools' gear) -- see
    drawButtonText().
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

    // A button whose text is one of the "icon:..." markers below (see EditorToolbar) gets a
    // drawn vector icon instead of literal text; anything else falls back to the normal
    // JUCE text rendering, so this is safe to use on every button, not just the transport.
    static constexpr const char* iconPlay          = "icon:play";
    static constexpr const char* iconStop          = "icon:stop";
    static constexpr const char* iconLoop          = "icon:loop";
    static constexpr const char* iconPlayFromStart = "icon:playFromStart";   // a bar + the play triangle
    static constexpr const char* iconTools         = "icon:tools";          // a gear/cog

    void drawButtonText(juce::Graphics&, juce::TextButton&,
                       bool isMouseOverButton, bool isButtonDown) override;

private:
    juce::SharedResourcePointer<ThemeManager> theme;
};
