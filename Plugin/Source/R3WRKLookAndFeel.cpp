#include "R3WRKLookAndFeel.h"
#include "BinaryData.h"
#include "DotMatrixLCD.h"
#include <cmath>

juce::Typeface::Ptr R3WRKLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    static juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor(
        BinaryData::SpaceMonoRegular_ttf, BinaryData::SpaceMonoRegular_ttfSize);
    static juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor(
        BinaryData::SpaceMonoBold_ttf, BinaryData::SpaceMonoBold_ttfSize);

    return font.isBold() ? bold : regular;
}

// Explicitly attaches the embedded Space Mono typeface (the same one getTypefaceForFont()
// above resolves to) to the returned Font, rather than just requesting a size/weight and
// relying on the app-wide default LookAndFeel to resolve it. Used anywhere that font needs to
// be guaranteed regardless of which LookAndFeel a component happens to inherit -- see
// createSliderTextBox() below, which every knob (KnobRow's own and the FX drawer panels'
// compact knobs alike) shares, so this is the one place that keeps their readouts identical.
juce::Font spaceMonoFont(float height, bool bold)
{
    static juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor(
        BinaryData::SpaceMonoRegular_ttf, BinaryData::SpaceMonoRegular_ttfSize);
    static juce::Typeface::Ptr boldTf = juce::Typeface::createSystemTypefaceFor(
        BinaryData::SpaceMonoBold_ttf, BinaryData::SpaceMonoBold_ttfSize);
    return juce::Font(juce::FontOptions(height).withTypeface(bold ? boldTf : regular));
}

// Explicitly attaches the OS's own system UI typeface -- bypassing getTypefaceForFont()'s
// Space Mono override, which reads poorly at the very small sizes a few badges/pills use (see
// KnobRow::ModelBadge's own comment for the story: confirmed by the user, extra kerning alone
// didn't fix it). The height passed to createSystemTypefaceFor() here is just metadata for
// resolving the right OS font by name/weight -- the typeface itself is scalable, so the actual
// rendered size always comes from the `height` passed to the returned Font.
juce::Font systemUIFont(float height, bool bold)
{
    static juce::Typeface::Ptr regular =
        juce::Typeface::createSystemTypefaceFor(juce::Font(juce::FontOptions(16.0f)));
    static juce::Typeface::Ptr boldTf =
        juce::Typeface::createSystemTypefaceFor(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    return juce::Font(juce::FontOptions(height).withTypeface(bold ? boldTf : regular));
}

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

    // A plain leftward arrow -- same shape/proportions as Reverse's own icon (see
    // drawReverseIcon below) -- for the reverse-loop state of the Loop button (off -> loop
    // -> ping-pong -> reverse -> off). Its own function, not a shared call, so the two can
    // still diverge later if reverse-loop turns out to need a visually distinct glyph.
    void drawLoopReverseIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.28f);
        const float shaftThickness = juce::jmax(1.6f, area.getHeight() * 0.28f);
        const float headLen        = area.getWidth() * 0.55f;
        const float headHalfHeight = area.getHeight() * 0.5f;

        juce::Path arrow;
        arrow.addTriangle(area.getX(),                area.getCentreY(),
                          area.getX() + headLen,      area.getCentreY() - headHalfHeight,
                          area.getX() + headLen,      area.getCentreY() + headHalfHeight);
        arrow.addRectangle(area.getX() + headLen * 0.55f, area.getCentreY() - shaftThickness * 0.5f,
                           area.getRight() - (area.getX() + headLen * 0.55f), shaftThickness);

        g.setColour(ink);
        g.fillPath(arrow);
    }

    // A continuous figure-eight (lemniscate) -- the "ping-pong loop" glyph. One stroke that
    // crosses itself at the centre, a right lobe then a left lobe, each traced with two cubics.
    void drawInfinityIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.30f);
        const auto c = area.getCentre();
        const float lobe = juce::jmin(area.getWidth() * 0.25f, area.getHeight() * 0.5f);
        const float thickness = juce::jmax(1.4f, lobe * 0.5f);

        juce::Path p;
        p.startNewSubPath(c.x, c.y);
        p.cubicTo(c.x + lobe * 0.55f, c.y - lobe, c.x + 2.0f * lobe, c.y - lobe, c.x + 2.0f * lobe, c.y);
        p.cubicTo(c.x + 2.0f * lobe, c.y + lobe, c.x + lobe * 0.55f, c.y + lobe, c.x, c.y);
        p.cubicTo(c.x - lobe * 0.55f, c.y - lobe, c.x - 2.0f * lobe, c.y - lobe, c.x - 2.0f * lobe, c.y);
        p.cubicTo(c.x - 2.0f * lobe, c.y + lobe, c.x - lobe * 0.55f, c.y + lobe, c.x, c.y);

        g.setColour(ink);
        g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
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

    // Clear: a plain X, traced from a reference icon the user supplied -- two crossing
    // diagonal strokes, rounded caps, same weight as the gear/reel icons' stroked lines.
    void drawClearIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.30f);
        const float thickness = juce::jmax(1.8f, area.getHeight() * 0.20f);

        juce::Path x;
        x.startNewSubPath(area.getX(), area.getY());
        x.lineTo(area.getRight(), area.getBottom());
        x.startNewSubPath(area.getRight(), area.getY());
        x.lineTo(area.getX(), area.getBottom());

        g.setColour(ink);
        g.strokePath(x, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
    }

    // Auto-Record: a gauge -- an outer ring, a shorter/thicker inner scale arc, and a needle
    // pointing off toward the upper right, traced from a reference icon the user supplied
    // (radial ray-cast the same way as the gear/reel icons: the ring is a plain circle
    // throughout, the scale arc spans roughly -95deg to +5deg -- lower-left, up through the
    // top, to just past it -- and the needle points to +45deg, well past the arc's own end,
    // exactly as measured in the reference rather than tidied into a symmetric sweep).
    void drawGaugeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto area = bounds.reduced(bounds.getHeight() * 0.16f);
        const auto  centre = area.getCentre();
        const float outerR = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;

        g.setColour(ink);

        const float ringThickness = outerR * 0.18f;
        g.drawEllipse(centre.x - outerR + ringThickness * 0.5f, centre.y - outerR + ringThickness * 0.5f,
                      (outerR - ringThickness * 0.5f) * 2.0f, (outerR - ringThickness * 0.5f) * 2.0f,
                      ringThickness);

        const float arcR = outerR * 0.66f;
        const float arcThickness = outerR * 0.19f;
        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f,
                          juce::degreesToRadians(-95.0f), juce::degreesToRadians(5.0f), true);
        g.strokePath(arc, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::butt));

        const float needleLen = outerR * 0.66f;
        const float needleThickness = juce::jmax(1.6f, outerR * 0.16f);
        const auto tip = centre.getPointOnCircumference(needleLen, juce::degreesToRadians(45.0f));
        g.drawLine(centre.x, centre.y, tip.x, tip.y, needleThickness);

        const float dotR = outerR * 0.19f;
        g.fillEllipse(centre.x - dotR, centre.y - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    // Slice: a marker flag -- a bold vertical pole with a square banner at the top. It's the
    // literal "drop a marker here" glyph, which is exactly what the Slice tool does. Based on a
    // reference icon the user supplied (minus its little "+" badge; banner squared off at their
    // request). All filled; built in nominal units, scaled to fill the button, centred by its
    // own bounds.
    void drawSliceIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        const float poleX     = -2.6f;
        const float top       = -8.5f;
        const float bot       =  8.5f;
        const float poleHalfW =  1.15f;
        const float flagW     =  8.2f;
        const float flagH     =  6.2f;

        juce::Path flag;

        // Pole: a filled bar with softly rounded ends.
        flag.addRoundedRectangle (poleX - poleHalfW, top, poleHalfW * 2.0f, bot - top, poleHalfW);

        // Banner: a plain rectangle hanging off the top of the pole.
        flag.addRectangle (poleX, top, flagW, flagH);

        const auto target = bounds.reduced (bounds.getHeight() * 0.22f);
        auto bb = flag.getBounds();
        const float k = juce::jmin (target.getWidth()  / bb.getWidth(),
                                    target.getHeight() / bb.getHeight());
        flag.applyTransform (juce::AffineTransform::scale (k, k));
        bb = flag.getBounds();
        flag.applyTransform (juce::AffineTransform::translation (
            bounds.getCentreX() - bb.getCentreX(), bounds.getCentreY() - bb.getCentreY()));

        g.setColour (ink);
        g.fillPath (flag);
    }

    // Follow-playhead: a playhead marker (a downward triangle head + a thin line down the
    // centre) flanked by two inward-pointing chevrons -- "keep the playhead in view". The
    // chevrons are the follow cue; ported loosely from Sieve's FollowPlayheadIcon.
    void drawFollowIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        // Tighter inset than the other icons: keeps the glyph detail large on the small
        // corner button.
        auto a = bounds.reduced (bounds.getHeight() * 0.15f);
        const float w = a.getWidth(), h = a.getHeight();
        const float cx = a.getCentreX();
        const float lineW = juce::jmax (1.4f, h * 0.085f);

        g.setColour (ink);

        // Playhead: triangle head at the top, thin line straight down.
        const float tw = w * 0.16f, td = h * 0.26f;
        juce::Path head;
        head.addTriangle (cx - tw, a.getY(), cx + tw, a.getY(), cx, a.getY() + td);
        g.fillPath (head);
        g.fillRect (cx - lineW * 0.5f, a.getY(), lineW, h);

        // Inward chevrons > ... < flanking the line, centred vertically.
        const float cy = a.getCentreY() + h * 0.06f;
        const float ch = h * 0.20f, reach = w * 0.40f;
        juce::Path chev;
        chev.startNewSubPath (cx - reach + ch, cy - ch);
        chev.lineTo          (cx - reach,      cy);
        chev.lineTo          (cx - reach + ch, cy + ch);
        chev.startNewSubPath (cx + reach - ch, cy - ch);
        chev.lineTo          (cx + reach,      cy);
        chev.lineTo          (cx + reach - ch, cy + ch);
        g.strokePath (chev, juce::PathStrokeType (juce::jmax (1.5f, h * 0.10f),
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    // Drawer toggle: a centre dot with two dashed "orbit" rings, each carrying one solid dot --
    // the r3wrk Component Library's solar-system glyph, picked by the user to replace the old
    // plain chevron. Geometry lifted proportionally from the reference sheet's 100x100 viewBox:
    // centre dot r=6, inner ring r=20 (dash 3/4) with a dot at 0deg, outer ring r=34 (dash 3/5)
    // with a dot at -90deg (top). While the drawer is OPEN, the outer ring and its planet drop
    // out -- same glyph, just the outermost element removed -- per the user's request, so the
    // closed-state icon reads as a clear subset of the open one instead of an unrelated plain dot.
    void drawOrbitIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink, bool open)
    {
        // The glyph's own extent (outer ring r=34 plus its planet dot) is a ~72-diameter circle,
        // already centred at (50,50) -- scaling by the raw 100-unit viewBox (as if whitespace
        // out to the edges were part of the icon) read small next to the knobs. Fit the actual
        // 72-diameter circle to the button instead, so it reads at roughly the knobs' own size.
        constexpr float glyphDiameter = 72.0f;
        constexpr float fillFraction = 0.90f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float s  = fillFraction * juce::jmin (bounds.getWidth(), bounds.getHeight()) / glyphDiameter;

        g.setColour (ink);

        g.fillEllipse (cx - 6.0f * s, cy - 6.0f * s, 12.0f * s, 12.0f * s);   // centre dot, r=6

        auto strokeDashedRing = [&] (float r, float dashOn, float dashOff)
        {
            juce::Path ring;
            ring.addEllipse (cx - r * s, cy - r * s, 2.0f * r * s, 2.0f * r * s);
            float dashes[2] = { dashOn * s, dashOff * s };
            juce::Path dashed;
            juce::PathStrokeType (juce::jmax (1.0f, 2.5f * s)).createDashedStroke (dashed, ring, dashes, 2);
            g.fillPath (dashed);
        };
        strokeDashedRing (20.0f, 3.0f, 4.0f);   // inner orbit, r=20
        g.fillEllipse (cx + 20.0f * s - 3.4f * s, cy - 3.4f * s, 6.8f * s, 6.8f * s);   // planet on inner orbit (0deg)

        if (open)
            return;   // outer orbit is the "more effects available" affordance -- dropped once open

        strokeDashedRing (34.0f, 3.0f, 5.0f);   // outer orbit, r=34
        g.fillEllipse (cx - 2.6f * s, cy - 34.0f * s - 2.6f * s, 5.2f * s, 5.2f * s);   // planet on outer orbit (-90deg/top)
    }

    // "More" (opens the full popup editor -- Mimeophon/Plexiphon/Reverb's compact panels): a
    // small outlined ring with a filled dot centred inside it, replacing the old "..." text.
    void drawMoreIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto a = bounds.reduced (bounds.getHeight() * 0.14f);   // was 0.28f -- read too small
        const float d = juce::jmin (a.getWidth(), a.getHeight());
        const float cx = a.getCentreX();
        const float cy = a.getCentreY();

        g.setColour (ink);
        g.drawEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d, juce::jmax (1.2f, d * 0.12f));

        const float dotR = d * 0.20f;
        g.fillEllipse (cx - dotR, cy - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    // Record Desktop (Standalone only): a computer-monitor outline (rounded screen + a short
    // stand) with a filled record dot centred on the screen -- "capture what's playing on
    // this Mac".
    void drawDesktopRecIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto a = bounds.reduced (bounds.getHeight() * 0.24f);
        const float w = a.getWidth(), h = a.getHeight();
        const float stroke = juce::jmax (1.4f, h * 0.09f);

        // Screen: a rounded rectangle taking the top ~72% of the icon box.
        juce::Rectangle<float> screen (a.getX(), a.getY(), w, h * 0.72f);
        g.setColour (ink);
        g.drawRoundedRectangle (screen.reduced (stroke * 0.5f), h * 0.10f, stroke);

        // Stand: a short neck + a base foot centred under the screen.
        const float cx = a.getCentreX();
        g.fillRect (cx - stroke * 0.6f, screen.getBottom(), stroke * 1.2f, h * 0.12f);
        g.fillRect (cx - w * 0.20f, a.getBottom() - stroke, w * 0.40f, stroke);

        // Record dot on the screen.
        const float r = juce::jmin (screen.getWidth(), screen.getHeight()) * 0.24f;
        g.fillEllipse (cx - r, screen.getCentreY() - r, r * 2.0f, r * 2.0f);
    }

    // Float on top: an eye -- pointed almond outline, an iris ring, a solid pupil. Flat line
    // art, no shading or highlight, from a reference picked by the user to replace the old
    // two-rings-with-arrows glyph.
    void drawFloatTopIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        // Almond is ~2.2:1, sized to the button the same way the other icons fill their frame.
        const float h = juce::jmin (bounds.getHeight() * 0.82f, bounds.getWidth() * 0.82f / 2.2f);
        const float w = h * 2.2f;
        const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
        const float stroke = juce::jmax (1.2f, h * 0.09f);

        // Two quadratic lids meeting in sharp corners. A quadratic's peak sits halfway to its
        // control point, so the control goes at a full h from centre for an h/2 lid height.
        juce::Path almond;
        almond.startNewSubPath (cx - w * 0.5f, cy);
        almond.quadraticTo (cx, cy - h, cx + w * 0.5f, cy);
        almond.quadraticTo (cx, cy + h, cx - w * 0.5f, cy);
        almond.closeSubPath();

        g.setColour (ink);
        g.strokePath (almond, juce::PathStrokeType (stroke, juce::PathStrokeType::mitered,
                                                    juce::PathStrokeType::rounded));

        const float irisR = h * 0.41f;
        g.drawEllipse (cx - irisR, cy - irisR, irisR * 2.0f, irisR * 2.0f, stroke);

        const float pupilR = h * 0.22f;
        g.fillEllipse (cx - pupilR, cy - pupilR, pupilR * 2.0f, pupilR * 2.0f);
    }

    // Capture Output: a record dot with a downward arrow beneath it -- "record what's coming
    // out, down to a file". Inked in the record red on the button.
    void drawCaptureOutIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto a = bounds.reduced (bounds.getHeight() * 0.24f);
        const float cx = a.getCentreX();
        const float stroke = juce::jmax (1.5f, a.getHeight() * 0.12f);

        g.setColour (ink);

        // Record dot, top.
        const float r = a.getWidth() * 0.18f;
        const float dotCy = a.getY() + r;
        g.fillEllipse (cx - r, dotCy - r, r * 2.0f, r * 2.0f);

        // Shaft + arrowhead, pointing down.
        const float shaftTop = dotCy + r + a.getHeight() * 0.08f;
        const float shaftBot = a.getBottom() - a.getHeight() * 0.24f;
        g.fillRect (cx - stroke * 0.5f, shaftTop, stroke, juce::jmax (0.0f, shaftBot - shaftTop));

        const float aw = a.getWidth() * 0.22f;
        juce::Path head;
        head.addTriangle (cx - aw, shaftBot, cx + aw, shaftBot, cx, a.getBottom());
        g.fillPath (head);
    }

    // Black Box: a rounded rectangle (the flight-recorder box) with a record dot inside --
    // reads as "quietly, continuously capturing" rather than an urgent record action, so it's
    // an outline only, no red -- see its neutral colour treatment in the .cpp constructor.
    void drawBlackBoxIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour ink)
    {
        auto a = bounds.reduced (bounds.getHeight() * 0.24f);
        const float stroke = juce::jmax (1.4f, a.getHeight() * 0.11f);

        g.setColour (ink);
        g.drawRoundedRectangle (a.reduced (stroke * 0.5f), a.getHeight() * 0.18f, stroke);

        const float r = juce::jmin (a.getWidth(), a.getHeight()) * 0.20f;
        g.fillEllipse (a.getCentreX() - r, a.getCentreY() - r, r * 2.0f, r * 2.0f);
    }
}

juce::Label* R3WRKLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox(slider);
    // Explicit typeface (not just a bigger size) -- see spaceMonoFont()'s own comment. Makes the
    // Space Mono attachment guaranteed rather than relying on getTypefaceForFont() being called
    // for this Label at all. Regular weight, not spaceMonoFont()'s bold default -- these readouts
    // were always the regular weight (implicit, via getTypefaceForFont()'s isBold() check on a
    // plain FontOptions(16.0f)); the bold Space Mono face's glyph metrics also render visibly
    // smaller at the same point size, so bold silently shrank and thickened the text at once.
    l->setFont(spaceMonoFont(16.0f, false));
    return l;
}

void R3WRKLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                        juce::Slider& slider)
{
    const auto& pal = theme->palette();

    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height).reduced(3.0f);
    const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto  centre   = bounds.getCentre();
    const float radius   = diameter * 0.5f;
    const float angle    = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // No filled disc -- just an outline ring in the accent colour, on whatever's behind it
    // (the knob row's own panel), plus a full-length clock-hand pointer from a small centre
    // gap out to near the rim -- matching a reference knob the user supplied (a plain
    // outlined circle + hand, no fill, no short rim tick, no centre dot). Both strokes go
    // bolder while the knob is actively being dragged, so turning one gives clear feedback
    // about which knob has the mouse.
    const juce::Colour ring = pal.accent;
    const bool active = slider.isMouseButtonDown();
    const float weight = active ? 1.6f : 1.0f;

    const juce::Rectangle<float> discBounds(centre.x - radius, centre.y - radius, diameter, diameter);
    g.setColour(ring);
    g.drawEllipse(discBounds.reduced(1.0f), juce::jmax(1.6f, radius * 0.09f) * weight);

    const float pointerThickness = juce::jmax(1.6f, radius * 0.14f) * weight;
    const float innerGap = radius * 0.18f;
    const float outerLen = radius * 0.82f;

    juce::Path pointerPath;
    pointerPath.addRoundedRectangle(-pointerThickness * 0.5f, -outerLen,
                                    pointerThickness, outerLen - innerGap, pointerThickness * 0.5f);
    pointerPath.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
    g.setColour(ring);
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
        // Record Desktop / Capture Output's red ring reads thicker than the other outline
        // buttons -- per the user's request, once their icons went white against it. Matched by
        // name (set in EditorToolbar's ctor) rather than icon text, since both buttons swap
        // their text to iconStop while active and text alone can't tell them apart from the
        // main transport's own Stop button.
        const bool thickRing = button.getName() == "desktopRec" || button.getName() == "captureOut"
                             || button.getName() == "clear";
        g.setColour(ink.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds, radius, thickRing ? 2.2f : 1.2f);
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
    if (text != iconPlay && text != iconStop && text != iconLoop && text != iconInfinity
        && text != iconLoopReverse
        && text != iconPlayFromStart && text != iconTools && text != iconScrub
        && text != iconReverse && text != iconClear && text != iconAutoRecord
        && text != iconSlice && text != iconFollow && text != iconDesktopRec
        && text != iconFloatTop && text != iconCaptureOut && text != iconBlackBox
        && text != iconOrbit && text != iconMore)
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
    if (text == iconInfinity)
    {
        drawInfinityIcon(g, bounds, ink);
        return;
    }
    if (text == iconLoopReverse)
    {
        drawLoopReverseIcon(g, bounds, ink);
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
    if (text == iconSlice)
    {
        drawSliceIcon(g, bounds, ink);
        return;
    }
    if (text == iconFollow)
    {
        drawFollowIcon(g, bounds, ink);
        return;
    }
    if (text == iconDesktopRec)
    {
        // White icon inside the red outline (drawButtonBackground's own outline still reads
        // textColourOffId/OnId, so the circle stays red) -- per the user's request, so the glyph
        // reads clearly against the button's own red ring instead of blending into it.
        drawDesktopRecIcon(g, bounds, juce::Colours::white.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
        return;
    }
    if (text == iconFloatTop)
    {
        drawFloatTopIcon(g, bounds, ink);
        return;
    }
    if (text == iconCaptureOut)
    {
        // See iconDesktopRec above -- same white-icon-in-red-ring treatment.
        drawCaptureOutIcon(g, bounds, juce::Colours::white.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
        return;
    }
    if (text == iconBlackBox)
    {
        drawBlackBoxIcon(g, bounds, ink);
        return;
    }
    if (text == iconOrbit)
    {
        drawOrbitIcon(g, bounds, ink, button.getToggleState());
        return;
    }
    if (text == iconMore)
    {
        drawMoreIcon(g, bounds, ink);
        return;
    }
    if (text == iconReverse)
    {
        drawReverseIcon(g, bounds, ink);
        return;
    }
    if (text == iconClear)
    {
        drawClearIcon(g, bounds, ink);
        return;
    }
    if (text == iconAutoRecord)
    {
        drawGaugeIcon(g, bounds, ink);
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

void R3WRKLookAndFeel::drawCallOutBoxBackground(juce::CallOutBox& box, juce::Graphics& g,
                                                const juce::Path& path, juce::Image& cachedImage)
{
    // Same drop-shadow caching LookAndFeel_V4's default uses; only the fill/outline colours
    // change, from JUCE's generic ColourScheme to this app's own theme.
    if (cachedImage.isNull())
    {
        cachedImage = juce::Image(juce::Image::ARGB, box.getWidth(), box.getHeight(), true,
                                  *g.getInternalContext().getPreferredImageTypeForTemporaryImages());
        cachedImage.setBackupEnabled(false);

        juce::Graphics g2(cachedImage);
        juce::DropShadow(juce::Colours::black.withAlpha(0.7f), 8, { 0, 2 }).drawForPath(g2, path);
    }

    g.setColour(juce::Colours::black);
    g.drawImageAt(cachedImage, 0, 0);

    const auto& pal = theme->palette();
    g.setColour(pal.popupBg.withAlpha(0.95f));
    g.fillPath(path);

    // The faint all-over dot grid visible in "blank" areas of a real LCD -- clipped to the
    // bubble's own path so it doesn't spill past the rounded corners.
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(path);
        lcd::drawScreenTexture(g, path.getBounds().toNearestInt(), pal.popupInk.withAlpha(0.10f));
    }

    g.setColour(pal.popupInk.withAlpha(0.6f));
    g.strokePath(path, juce::PathStrokeType(2.0f));
}
