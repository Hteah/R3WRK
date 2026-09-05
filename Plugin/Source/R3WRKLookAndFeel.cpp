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

    // Tools: a plain 6-tooth gear, traced from a reference icon the user supplied
    // rather than approximated by formula. kOuterR/kInnerR are radius ratios
    // (of the tooth-tip radius) sampled directly from that image via radial
    // ray-casting at 7.5deg steps around one 60deg sector, averaged across all
    // six sectors (the source is exactly 6-fold symmetric) -- see PROJECT_NOTES.md
    // for how these were extracted. Each ring (the gear body and the centre hole)
    // is traced as an outer contour (clockwise) plus an inner contour
    // (counter-clockwise), so nonzero-winding fillPath punches the band out
    // directly. That reproduces the reference's actual ring width, which isn't
    // constant -- it pinches in at each tooth tip and at each valley, and swells
    // on the flanks between -- rather than the constant-width band a plain
    // centreline stroke would give.
    void drawGearIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        static constexpr float kOuterR[8] = { 0.9933f, 0.9717f, 0.9133f, 0.7700f,
                                              0.7122f, 0.7700f, 0.9133f, 0.9717f };
        static constexpr float kInnerR[8] = { 0.7867f, 0.7206f, 0.5595f, 0.5227f,
                                              0.5189f, 0.5228f, 0.5605f, 0.7206f };
        constexpr int   kPerSector  = 8;
        constexpr int   kTotal      = kPerSector * 6;   // 48 points around the full gear
        constexpr float kHoleOuterR = 0.366f;
        constexpr float kHoleInnerR = 0.166f;

        auto area = bounds.reduced(bounds.getHeight() * 0.20f);
        const float R = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        const auto  centre = area.getCentre();
        const float step = juce::MathConstants<float>::twoPi / (float) kTotal;

        juce::Path gear;

        for (int i = 0; i < kTotal; ++i)
        {
            const float angle = (float) i * step;
            auto p = centre.getPointOnCircumference(R * kOuterR[i % kPerSector], angle);
            if (i == 0) gear.startNewSubPath(p); else gear.lineTo(p);
        }
        gear.closeSubPath();

        for (int i = kTotal; i > 0; --i)   // reverse order -> opposite (counter-clockwise) winding
        {
            const int idx = i % kTotal;
            const float angle = (float) idx * step;
            auto p = centre.getPointOnCircumference(R * kInnerR[idx % kPerSector], angle);
            if (i == kTotal) gear.startNewSubPath(p); else gear.lineTo(p);
        }
        gear.closeSubPath();

        gear.addCentredArc(centre.x, centre.y, R * kHoleOuterR, R * kHoleOuterR, 0.0f,
                           0.0f, juce::MathConstants<float>::twoPi, true);
        gear.addCentredArc(centre.x, centre.y, R * kHoleInnerR, R * kHoleInnerR, 0.0f,
                           juce::MathConstants<float>::twoPi, 0.0f, true);

        g.setColour(ink);
        g.fillPath(gear);
    }

    // Scrub: a reel hub, traced from a reference icon the user supplied -- a solid ring whose
    // inner hole isn't a smooth circle but a 6-point spline (six rectangular notches, 60°
    // apart, cut inward like a cassette reel's drive hub, the little sprocketed hole a tape
    // deck's spindle grips to turn the reel). Same "outer contour clockwise + inner contour
    // counter-clockwise, nonzero winding punches the hole" technique as drawGearIcon's body/
    // centre-hole, just applied to a spline hole instead of a round one.
    void drawScrubIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        const auto area   = bounds.reduced(bounds.getHeight() * 0.14f);
        const auto centre = area.getCentre();
        const float outerR     = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        const float ringInnerR = outerR * 0.79f;   // hole radius between notches
        const float toothR     = outerR * 0.59f;   // hole radius at each notch (cut deeper)

        constexpr int   numTeeth          = 6;
        constexpr float toothHalfWidthDeg = 9.0f;
        constexpr float stepDeg           = 360.0f / (float) numTeeth;
        auto angleAt = [](float deg) { return juce::degreesToRadians(deg); };

        // Outer boundary: a plain circle, clockwise (increasing angle is JUCE's clockwise).
        juce::Path ring;
        ring.addCentredArc(centre.x, centre.y, outerR, outerR, 0.0f,
                           0.0f, juce::MathConstants<float>::twoPi, true);

        // Hole boundary: the six-notch spline, traced counter-clockwise (decreasing angle,
        // walking teeth from the highest index down to 0) so nonzero-winding punches it out
        // of the ring above as a hole rather than adding to it.
        const float firstRightEdge = (float) (numTeeth - 1) * stepDeg + toothHalfWidthDeg;
        juce::Path hole;
        bool started = false;
        for (int i = numTeeth - 1; i >= 0; --i)
        {
            const float toothCentre = (float) i * stepDeg;
            const float leftEdge  = toothCentre - toothHalfWidthDeg;
            const float rightEdge = toothCentre + toothHalfWidthDeg;

            const auto rightOuter = centre.getPointOnCircumference(ringInnerR, angleAt(rightEdge));
            const auto rightInner = centre.getPointOnCircumference(toothR,     angleAt(rightEdge));
            const auto leftOuter  = centre.getPointOnCircumference(ringInnerR, angleAt(leftEdge));

            if (! started) { hole.startNewSubPath(rightOuter); started = true; }
            else             hole.lineTo(rightOuter);

            hole.lineTo(rightInner);
            hole.addCentredArc(centre.x, centre.y, toothR, toothR, 0.0f,
                               angleAt(rightEdge), angleAt(leftEdge), false);
            hole.lineTo(leftOuter);

            // Sweep the gap at ringInnerR down to the next notch's right edge -- the previous
            // tooth in walking order, wrapping past 0 back to the very first point once i == 0.
            const float nextRightEdge = (i > 0) ? ((float) (i - 1) * stepDeg + toothHalfWidthDeg)
                                                 : (firstRightEdge - 360.0f);
            hole.addCentredArc(centre.x, centre.y, ringInnerR, ringInnerR, 0.0f,
                               angleAt(leftEdge), angleAt(nextRightEdge), false);
        }
        hole.closeSubPath();

        juce::Path full;
        full.addPath(ring);
        full.addPath(hole);
        g.setColour(ink);
        g.fillPath(full);
    }

    // Reverse: a plain leftward arrow -- a triangular head at the left end joined to a
    // horizontal shaft, distinct from Play's plain triangle (which points the other way and
    // already means something else) so the two aren't mistaken for mirror images of the
    // same action.
    void drawReverseIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.28f);
        const float shaftThickness = juce::jmax(1.6f, area.getHeight() * 0.28f);
        const float headLen        = area.getWidth() * 0.55f;
        const float headHalfHeight = area.getHeight() * 0.5f;

        juce::Path arrow;
        arrow.addTriangle(area.getX(),                area.getCentreY(),
                          area.getX() + headLen,      area.getCentreY() - headHalfHeight,
                          area.getX() + headLen,      area.getCentreY() + headHalfHeight);
        // Overlaps the head slightly so the join reads as one continuous arrow, not two shapes.
        arrow.addRectangle(area.getX() + headLen * 0.55f, area.getCentreY() - shaftThickness * 0.5f,
                           area.getRight() - (area.getX() + headLen * 0.55f), shaftThickness);

        g.setColour(ink);
        g.fillPath(arrow);
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
        && text != iconPlayFromStart && text != iconTools && text != iconScrub
        && text != iconReverse)
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
    if (text == iconScrub)
    {
        drawScrubIcon(g, bounds, ink);
        return;
    }
    if (text == iconReverse)
    {
        drawReverseIcon(g, bounds, ink);
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
