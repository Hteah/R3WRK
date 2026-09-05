#include "R3WRKLookAndFeel.h"

void R3WRKLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                        juce::Slider&)
{
    const auto& pal = theme->palette();

    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height).reduced(3.0f);
    const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto  centre   = bounds.getCentre();
    const float radius   = diameter * 0.5f;
    const float angle    = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Disc: the panel colour, darkened for weight/contrast -- so it visibly tracks each
    // theme (dark blue-grey in Midnight, dark warm brown in Amber, a muted tan in the
    // light Paper theme) rather than staying a fixed near-black regardless of theme.
    const juce::Colour disc    = pal.panelBg.interpolatedWith(juce::Colours::black, 0.35f);
    const juce::Colour outline = pal.text.withAlpha(0.35f);
    const juce::Colour pointer = pal.accent;

    const juce::Rectangle<float> discBounds(centre.x - radius, centre.y - radius, diameter, diameter);
    g.setColour(disc);
    g.fillEllipse(discBounds);
    g.setColour(outline);
    g.drawEllipse(discBounds.reduced(0.75f), 1.5f);

    // A short tick near the rim, not a long line through the middle.
    juce::Path pointerPath;
    const float pointerLen       = radius * 0.30f;
    const float pointerThickness = juce::jmax(1.6f, radius * 0.16f);
    pointerPath.addRoundedRectangle(-pointerThickness * 0.5f, -radius * 0.88f,
                                    pointerThickness, pointerLen, pointerThickness * 0.5f);
    pointerPath.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
    g.setColour(pointer);
    g.fillPath(pointerPath);
}

// Pill buttons: fully rounded (radius = half the button height). A button whose configured
// background is fully transparent (see EditorToolbar::applyTheme -- that's how a component
// asks for the "outline" treatment rather than a filled one) gets a hairline border and a
// faint hover/press wash instead of a solid fill.
void R3WRKLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& backgroundColour,
                                            bool isHighlighted, bool isDown)
{
    const auto& pal = theme->palette();
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = bounds.getHeight() * 0.5f;

    if (backgroundColour.getAlpha() == 0)
    {
        if (isDown || isHighlighted)
        {
            g.setColour(pal.text.withAlpha(isDown ? 0.16f : 0.08f));
            g.fillRoundedRectangle(bounds, radius);
        }
        g.setColour(pal.textDim.withAlpha(0.55f));
        g.drawRoundedRectangle(bounds, radius, 1.2f);
        return;
    }

    juce::Colour fill = backgroundColour;
    if (isDown)             fill = fill.darker(0.18f);
    else if (isHighlighted) fill = fill.brighter(0.08f);
    if (! button.isEnabled()) fill = fill.withAlpha(0.4f);

    g.setColour(fill);
    g.fillRoundedRectangle(bounds, radius);
}
