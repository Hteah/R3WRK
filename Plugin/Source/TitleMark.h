#pragma once
#include <JuceHeader.h>
#include "Theme.h"

/**
    The "R3WRK" wordmark centred at the top of the editor -- custom lettering drawn as vector
    paths (no font file), in the spirit of late-'70s sci-fi magazine mastheads: monoline, wide,
    geometric, round-shouldered R / 3, a short-middle W.

    Glyphs are designed on a 10-unit cap height. Each letter is a stroked centreline
    (mitered joins, butt caps) and the whole word is clipped to the cap band, which is what
    squares off the diagonal ends of W / K / the R's leg -- so the band is snapped to whole
    pixels in paint() to keep those cuts crisp.

    Purely decorative: it never takes clicks, so the Standalone title-bar drag/zoom
    (PluginEditor::isInTitleArea) still works across it. Ink = Palette::text.
*/
class TitleMark : public juce::Component,
                  private juce::ChangeListener
{
public:
    TitleMark()
    {
        buildWordmark();
        setInterceptsMouseClicks(false, false);
        theme->addChangeListener(this);
    }

    ~TitleMark() override { theme->removeChangeListener(this); }

    // Cap height in px. Width follows from it (see getPreferredWidth()).
    void setCapHeight(int px) { capHeight = juce::jmax(6, px); repaint(); }
    int getCapHeight() const noexcept { return capHeight; }
    int getPreferredWidth() const { return (int) std::ceil(wordWidth * (float) capHeight / kCap); }

    void paint(juce::Graphics& g) override
    {
        const float scale = (float) capHeight / kCap;
        const float inkW  = wordWidth * scale;
        const float x0    = std::round(((float) getWidth() - inkW) * 0.5f);
        const int   y0    = (getHeight() - capHeight) / 2;   // whole pixel: crisp clipped ends

        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(juce::Rectangle<int>(0, y0, getWidth(), capHeight));
        g.setColour(theme->palette().text);
        g.fillPath(wordmark, juce::AffineTransform::scale(scale).translated(x0, (float) y0));
    }

private:
    static constexpr float kCap    = 10.0f;   // design cap height (units)
    static constexpr float kStroke = 1.7f;    // stroke width (units)
    static constexpr float kGap    = 3.0f;    // space between letter advances (units)

    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    // Centreline paths per glyph, in units, origin at the glyph's top-left on the cap band.
    // Ends that run past 0..kCap are deliberate -- the clip in paint() cuts them flat.
    static juce::Path glyphR(float& advance)
    {
        constexpr float h  = kStroke * 0.5f;
        constexpr float bowlBottom = 6.2f;                    // ink bottom of the bowl
        constexpr float r  = (bowlBottom - kStroke) * 0.5f;   // bowl centreline radius
        advance = 11.0f;
        const float xc = advance - h - r;

        juce::Path p;
        p.startNewSubPath(h, -1.0f);  p.lineTo(h, kCap + 1.0f);           // stem
        p.startNewSubPath(h, h);      p.lineTo(xc, h);                    // bowl
        p.addCentredArc(xc, h + r, r, r, 0.0f, 0.0f, juce::MathConstants<float>::pi, false);
        p.lineTo(h, bowlBottom - h);
        p.startNewSubPath(6.2f, bowlBottom - h);  p.lineTo(11.6f, 11.8f); // leg
        return p;
    }

    static juce::Path glyph3(float& advance)
    {
        constexpr float h = kStroke * 0.5f;
        constexpr float r = (kCap * 0.5f - h) * 0.5f;   // both bowls, centreline radius
        constexpr float mid = kCap * 0.5f;
        advance = 10.0f;
        const float xc = advance - h - r;
        constexpr float pi = juce::MathConstants<float>::pi;

        juce::Path p;
        p.startNewSubPath(0.0f, h);   p.lineTo(xc, h);                    // top bar + upper bowl
        p.addCentredArc(xc, h + r, r, r, 0.0f, 0.0f, pi, false);
        p.lineTo(3.4f, mid);                                              // short middle bar
        p.startNewSubPath(xc, mid);                                       // lower bowl + base
        p.addCentredArc(xc, mid + r, r, r, 0.0f, 0.0f, pi, false);
        p.lineTo(0.0f, kCap - h);
        return p;
    }

    static juce::Path glyphW(float& advance)
    {
        advance = 15.0f;
        juce::Path p;   // outer strokes to the baseline, middle apex stops short (y = 4)
        p.startNewSubPath(0.6f, -1.6f);
        p.lineTo(4.0f, 11.6f);
        p.lineTo(7.5f, 4.0f);
        p.lineTo(11.0f, 11.6f);
        p.lineTo(14.4f, -1.6f);
        return p;
    }

    static juce::Path glyphK(float& advance)
    {
        constexpr float h = kStroke * 0.5f;
        advance = 10.6f;
        juce::Path p;
        p.startNewSubPath(h, -1.0f);  p.lineTo(h, kCap + 1.0f);           // stem
        p.startNewSubPath(11.2f, -1.4f);                                  // arms, meeting in the stem
        p.lineTo(kStroke + 0.4f, kCap * 0.5f);
        p.lineTo(11.2f, 11.4f);
        return p;
    }

    void buildWordmark()
    {
        const juce::PathStrokeType stroke(kStroke, juce::PathStrokeType::mitered,
                                          juce::PathStrokeType::butt);
        float x = 0.0f;
        for (auto c : { 'R', '3', 'W', 'R', 'K' })
        {
            float advance = 0.0f;
            const auto centreline = c == 'R' ? glyphR(advance)
                                  : c == '3' ? glyph3(advance)
                                  : c == 'W' ? glyphW(advance)
                                             : glyphK(advance);
            juce::Path outline;
            stroke.createStrokedPath(outline, centreline);
            wordmark.addPath(outline, juce::AffineTransform::translation(x, 0.0f));
            x += advance + kGap;
        }
        wordWidth = x - kGap;
    }

    juce::SharedResourcePointer<ThemeManager> theme;
    juce::Path wordmark;     // filled outlines, design units
    float wordWidth = 0.0f;  // units
    int capHeight = 13;      // px

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TitleMark)
};
