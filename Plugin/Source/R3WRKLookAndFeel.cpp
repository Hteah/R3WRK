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

    // Tools: a plain gear/cog, line-icon style (stroked outline, hollow) to match a
    // reference glyph the user supplied -- same body+teeth silhouette as before, but
    // stroked with rounded joints instead of filled, so the tooth corners come out
    // rounded for free (PathStrokeType::curved), and a separate stroked circle stands
    // in for the centre hole instead of an even-odd punch-out.
    void drawGearIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.14f);   // as large as the button allows
        const float R = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        const auto  centre = area.getCentre();
        const float thickness = juce::jmax(1.3f, R * 0.15f);

        const float bodyR   = R * 0.44f;   // radius to the tooth root
        const float toothLen = R * 0.30f;  // tooth radial length
        const float holeR   = R * 0.30f;   // centre hole radius -- comfortably inside bodyR so
                                            // the ring band reads as hollow, not solid, once stroked
        const int   numTeeth = 6;   // matches the reference glyph (6 teeth, not 8)
        // Half-step for 6 teeth is 30deg -- keep well under that so each tooth stays a
        // distinct block with a clear gap either side, rather than merging into a ring.
        const float toothHalfWidth = juce::degreesToRadians(15.0f);
        const float step = juce::MathConstants<float>::twoPi / (float) numTeeth;

        juce::Path gear;
        bool first = true;
        for (int i = 0; i < numTeeth; ++i)
        {
            const float toothCentre = (float) i * step;
            const float toothL = toothCentre - toothHalfWidth;
            const float toothR = toothCentre + toothHalfWidth;
            const float gapEnd = toothCentre + step - toothHalfWidth;

            auto p1 = centre.getPointOnCircumference(bodyR,            toothL);
            auto p2 = centre.getPointOnCircumference(bodyR + toothLen, toothL);
            auto p3 = centre.getPointOnCircumference(bodyR + toothLen, toothR);
            auto p4 = centre.getPointOnCircumference(bodyR,            toothR);
            auto p5 = centre.getPointOnCircumference(bodyR,            gapEnd);

            if (first) { gear.startNewSubPath(p1); first = false; }
            else         gear.lineTo(p1);
            gear.lineTo(p2);
            gear.lineTo(p3);
            gear.lineTo(p4);
            gear.lineTo(p5);
        }
        gear.closeSubPath();

        g.setColour(ink);
        g.strokePath(gear, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
        g.drawEllipse(centre.x - holeR, centre.y - holeR, holeR * 2.0f, holeR * 2.0f, thickness);
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
        drawGearIcon(g, bounds, ink);
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
