#include "R3WRKLookAndFeel.h"
#include <cmath>

namespace
{
    // A small triangular arrowhead, tip at `angleDeg` (0 = up, clockwise) on the circle
    // of the given radius/centre, pointing in the clockwise direction of travel.
    void addLoopArrowhead(juce::Path& path, juce::Point<float> centre, float radius,
                          float angleDeg, float size)
    {
        const float angleRad = juce::degreesToRadians(angleDeg);
        const juce::Point<float> tip = centre.getPointOnCircumference(radius, angleRad);
        const juce::Point<float> tangent { std::cos(angleRad), std::sin(angleRad) };   // clockwise direction
        const juce::Point<float> normal  { -tangent.y, tangent.x };
        const juce::Point<float> back = tip - tangent * size;
        path.addTriangle(tip, back + normal * (size * 0.62f), back - normal * (size * 0.62f));
    }

    // Two ~140deg arcs with gaps between them, each ending in an arrowhead -- the classic
    // "loop / repeat" glyph, drawn rather than relying on a font (most "two arrows in a
    // circle" Unicode glyphs render as fixed-colour emoji and ignore the ink colour).
    void drawLoopIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.28f);
        const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        const auto  centre = area.getCentre();
        const float thickness = juce::jmax(1.4f, radius * 0.32f);

        juce::Path arcs;
        arcs.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                           juce::degreesToRadians(20.0f), juce::degreesToRadians(160.0f), true);
        arcs.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                           juce::degreesToRadians(200.0f), juce::degreesToRadians(340.0f), true);
        g.setColour(ink);
        g.strokePath(arcs, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        juce::Path heads;
        addLoopArrowhead(heads, centre, radius, 160.0f, radius * 0.55f);
        addLoopArrowhead(heads, centre, radius, 340.0f, radius * 0.55f);
        g.fillPath(heads);
    }

    // Tools: a crossed open-end wrench and flathead screwdriver, drawn as line art
    // (stroked outlines, nothing filled) to match the reference icon rather than the
    // solid-fill style of Play/Stop -- built in local (0,0)-centred coordinates with
    // the long axis along +x, then rotated into place, the same way as the rotary
    // knob's pointer tick.
    void drawToolsIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        // Small (28px) icon, so this simplifies the reference glyph to one terminal
        // shape per tool rather than two -- a single-jaw wrench and a plain-handled
        // screwdriver read far more clearly at this size than a fully symmetric,
        // four-shape version does.
        auto area = bounds.reduced(bounds.getHeight() * 0.22f);
        const float R = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        const auto  centre = area.getCentre();
        const float thickness = juce::jmax(1.6f, R * 0.24f);
        const float armLen = R * 1.6f;

        // Wrench: a shaft ending in one open ("C") jaw, gap facing outward -- same
        // addCentredArc-with-a-gap technique as the loop icon's arcs, but with a wide
        // (~120deg) gap so it reads as a clear open hook rather than a near-closed ring.
        juce::Path wrench;
        wrench.startNewSubPath(-armLen * 0.5f, 0.0f);
        wrench.lineTo(armLen * 0.30f, 0.0f);

        const float headR = R * 0.36f;
        const float gapHalf = 60.0f;
        const float from = juce::degreesToRadians(90.0f + gapHalf);
        const float to   = juce::degreesToRadians(90.0f + gapHalf + (360.0f - 2.0f * gapHalf));
        wrench.addCentredArc(armLen * 0.5f, 0.0f, headR, headR, 0.0f, from, to, true);

        wrench.applyTransform(juce::AffineTransform::rotation(juce::degreesToRadians(45.0f)).translated(centre));
        g.setColour(ink);
        g.strokePath(wrench, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        // Screwdriver: a shaft running to a point (the blade) plus a solid, filled
        // capsule handle at the other end -- a filled block reads more clearly than a
        // thin outline at this size.
        juce::Path driver;
        driver.startNewSubPath(-armLen * 0.5f, 0.0f);
        driver.lineTo(armLen * 0.14f, 0.0f);
        driver.applyTransform(juce::AffineTransform::rotation(juce::degreesToRadians(-45.0f)).translated(centre));
        g.strokePath(driver, juce::PathStrokeType(thickness * 0.85f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        juce::Rectangle<float> handle(armLen * 0.12f, -R * 0.30f, armLen * 0.42f, R * 0.60f);
        juce::Path handlePath;
        handlePath.addRoundedRectangle(handle, R * 0.20f);
        handlePath.applyTransform(juce::AffineTransform::rotation(juce::degreesToRadians(-45.0f)).translated(centre));
        g.fillPath(handlePath);
    }
}

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
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = bounds.getHeight() * 0.5f;

    if (backgroundColour.getAlpha() == 0)
    {
        // Outline style: take the ink colour from whatever text colour the button's owner
        // already set for it, rather than reading the theme directly -- this LookAndFeel is
        // shared by components that sit on different backgrounds (e.g. KnobRow's captions
        // sit on windowBg, EditorToolbar's outlined buttons sit on the dark control band /
        // panelBg), and each owner already picks the right pair for its own context.
        const juce::Colour ink = button.findColour(juce::TextButton::textColourOffId);
        if (isDown || isHighlighted)
        {
            g.setColour(ink.withAlpha(isDown ? 0.16f : 0.08f));
            g.fillRoundedRectangle(bounds, radius);
        }
        g.setColour(ink.withAlpha(0.45f));
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

void R3WRKLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                      bool isMouseOverButton, bool isButtonDown)
{
    const auto text = button.getButtonText();
    if (text != iconPlay && text != iconStop && text != iconLoop
        && text != iconPlayFromStart && text != iconTools)
    {
        juce::LookAndFeel_V4::drawButtonText(g, button, isMouseOverButton, isButtonDown);
        return;
    }

    const auto bounds = button.getLocalBounds().toFloat();
    const juce::Colour ink = button.findColour(button.getToggleState()
                                                  ? juce::TextButton::textColourOnId
                                                  : juce::TextButton::textColourOffId)
                                  .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f);

    if (text == iconLoop)
    {
        drawLoopIcon(g, bounds, ink);
        return;
    }
    if (text == iconTools)
    {
        drawToolsIcon(g, bounds, ink);
        return;
    }

    g.setColour(ink);
    if (text == iconPlay)
    {
        auto r = bounds.reduced(bounds.getHeight() * 0.32f);
        juce::Path p;
        p.addTriangle(r.getX(), r.getY(), r.getX(), r.getBottom(), r.getRight(), r.getCentreY());
        g.fillPath(p);
    }
    else if (text == iconPlayFromStart)
    {
        // A vertical bar, then the same play triangle shifted over to make room for it --
        // "go back to the start, then play forward".
        auto r = bounds.reduced(bounds.getHeight() * 0.28f);
        const float barWidth = juce::jmax(1.6f, r.getWidth() * 0.16f);
        const float gap      = r.getWidth() * 0.14f;

        g.fillRoundedRectangle(r.getX(), r.getY(), barWidth, r.getHeight(), barWidth * 0.4f);

        auto tri = r.withTrimmedLeft(barWidth + gap);
        juce::Path p;
        p.addTriangle(tri.getX(), tri.getY(), tri.getX(), tri.getBottom(), tri.getRight(), tri.getCentreY());
        g.fillPath(p);
    }
    else // iconStop
    {
        auto r = bounds.reduced(bounds.getHeight() * 0.34f);
        g.fillRoundedRectangle(r, 2.0f);
    }
}
