#include "DotMatrixLCD.h"

namespace lcd
{
    namespace
    {
        constexpr Glyph G(uint8_t r0, uint8_t r1, uint8_t r2, uint8_t r3, uint8_t r4, uint8_t r5, uint8_t r6)
        {
            return { r0, r1, r2, r3, r4, r5, r6 };
        }
    }

    const Glyph* findGlyph(juce::juce_wchar cIn) noexcept
    {
        static const Glyph kSpace = G(0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000);
        static const Glyph kDot   = G(0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100);
        static const Glyph kDash  = G(0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000);
        static const Glyph kPlus  = G(0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000);
        static const Glyph kPct   = G(0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011);
        static const Glyph kColon = G(0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000);
        static const Glyph kSlash = G(0b00001, 0b00010, 0b00100, 0b00100, 0b00100, 0b01000, 0b10000);
        static const Glyph kEquals = G(0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000);
        static const Glyph kApos   = G(0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000);
        static const Glyph kLParen = G(0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010);
        static const Glyph kRParen = G(0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000);
        static const Glyph kComma  = G(0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000);
        static const Glyph kMidDot = G(0b00000, 0b00000, 0b00000, 0b01100, 0b01100, 0b00000, 0b00000);
        static const Glyph kCmd    = G(0b11011, 0b11111, 0b01010, 0b01010, 0b01010, 0b11111, 0b11011);
        static const Glyph kShift  = G(0b00100, 0b01010, 0b10001, 0b11011, 0b01010, 0b01010, 0b01110);
        static const Glyph kAmp   = G(0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101);

        static const Glyph k0 = G(0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110);
        static const Glyph k1 = G(0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110);
        static const Glyph k2 = G(0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111);
        static const Glyph k3 = G(0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110);
        static const Glyph k4 = G(0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010);
        static const Glyph k5 = G(0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110);
        static const Glyph k6 = G(0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110);
        static const Glyph k7 = G(0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000);
        static const Glyph k8 = G(0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110);
        static const Glyph k9 = G(0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100);

        static const Glyph kA = G(0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001);
        static const Glyph kB = G(0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110);
        static const Glyph kC = G(0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110);
        static const Glyph kD = G(0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100);
        static const Glyph kE = G(0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111);
        static const Glyph kF = G(0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000);
        static const Glyph kGl = G(0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111);
        static const Glyph kH = G(0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001);
        static const Glyph kI = G(0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110);
        static const Glyph kJ = G(0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100);
        static const Glyph kK = G(0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001);
        static const Glyph kL = G(0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111);
        static const Glyph kM = G(0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001);
        static const Glyph kN = G(0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001);
        static const Glyph kO = G(0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110);
        static const Glyph kP = G(0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000);
        static const Glyph kQ = G(0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101);
        static const Glyph kR = G(0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001);
        static const Glyph kS = G(0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110);
        static const Glyph kT = G(0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100);
        static const Glyph kU = G(0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110);
        static const Glyph kV = G(0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100);
        static const Glyph kW = G(0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010);
        static const Glyph kX = G(0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001);
        static const Glyph kY = G(0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100);
        static const Glyph kZ = G(0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111);

        switch (juce::CharacterFunctions::toUpperCase(cIn))
        {
            case ' ': return &kSpace;
            case '.': return &kDot;
            case '-': return &kDash;
            case '+': return &kPlus;
            case '%': return &kPct;
            case ':': return &kColon;
            case '/': return &kSlash;
            case '&': return &kAmp;
            case '=': return &kEquals;
            case '(': return &kLParen;
            case ')': return &kRParen;
            case ',': return &kComma;
            case 0x00B7: return &kMidDot;   // ·
            case 0x2318: return &kCmd;      // ⌘
            case 0x21E7: return &kShift;    // ⇧
            case '\'': return &kApos;
            case '0': return &k0; case '1': return &k1; case '2': return &k2;
            case '3': return &k3; case '4': return &k4; case '5': return &k5;
            case '6': return &k6; case '7': return &k7; case '8': return &k8;
            case '9': return &k9;
            case 'A': return &kA; case 'B': return &kB; case 'C': return &kC;
            case 'D': return &kD; case 'E': return &kE; case 'F': return &kF;
            case 'G': return &kGl; case 'H': return &kH; case 'I': return &kI;
            case 'J': return &kJ; case 'K': return &kK; case 'L': return &kL;
            case 'M': return &kM; case 'N': return &kN; case 'O': return &kO;
            case 'P': return &kP; case 'Q': return &kQ; case 'R': return &kR;
            case 'S': return &kS; case 'T': return &kT; case 'U': return &kU;
            case 'V': return &kV; case 'W': return &kW; case 'X': return &kX;
            case 'Y': return &kY; case 'Z': return &kZ;
            default: return nullptr;
        }
    }

    // One glyph per character, so an ellipsis character (menu items' "Open…") becomes three dots.
    static juce::String normalise(const juce::String& text)
    {
        return text.toUpperCase().replace(juce::String::fromUTF8("\xe2\x80\xa6"), "...");
    }

    float textWidth(const juce::String& text, float dotSize) noexcept
    {
        const int n = normalise(text).length();
        if (n <= 0)
            return 0.0f;
        return (float) (n * 6 - 1) * dotSize;   // 5 dots/char + 1 dot gap, no trailing gap
    }

    void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                 float dotSize, juce::Colour ink, juce::Justification justification)
    {
        const auto upper = normalise(text);
        const float w = textWidth(upper, dotSize);
        const float h = 7.0f * dotSize;

        float x = area.getX();
        if (justification.testFlags(juce::Justification::horizontallyCentred))
            x = area.getCentreX() - w * 0.5f;
        else if (justification.testFlags(juce::Justification::right))
            x = area.getRight() - w;

        float y = area.getY();
        if (justification.testFlags(juce::Justification::verticallyCentred))
            y = area.getCentreY() - h * 0.5f;
        else if (justification.testFlags(juce::Justification::bottom))
            y = area.getBottom() - h;

        g.setColour(ink);
        const float dotW = dotSize * 0.85f;
        const float corner = dotSize * 0.25f;

        float cx = x;
        for (int i = 0; i < upper.length(); ++i)
        {
            if (const auto* glyph = findGlyph(upper[i]))
            {
                for (int row = 0; row < 7; ++row)
                {
                    const uint8_t bits = (*glyph)[(size_t) row];
                    for (int col = 0; col < 5; ++col)
                    {
                        if ((bits & (1u << (4 - col))) != 0)
                            g.fillRoundedRectangle(cx + (float) col * dotSize, y + (float) row * dotSize,
                                                   dotW, dotW, corner);
                    }
                }
            }
            cx += 6.0f * dotSize;
        }
    }

    void drawKnob(juce::Graphics& g, juce::Rectangle<float> bounds, float angle, juce::Colour ink)
    {
        const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const auto centre = bounds.getCentre();
        const float radius = diameter * 0.5f;
        const float dotSize = juce::jmax(1.5f, diameter * 0.09f);

        g.setColour(ink);

        // Ring: dots evenly spaced around the circle.
        constexpr int kRingDots = 18;
        for (int i = 0; i < kRingDots; ++i)
        {
            const float ringAngle = (float) i / (float) kRingDots * juce::MathConstants<float>::twoPi;
            const auto p = centre.getPointOnCircumference(radius - dotSize * 0.5f, ringAngle);
            g.fillEllipse(p.x - dotSize * 0.5f, p.y - dotSize * 0.5f, dotSize, dotSize);
        }

        // Pointer: a short radial trail of dots out to the rim at `angle`, plus a centre dot --
        // the same "circle + pointer dot" language the reference hardware's own rotary-value
        // icons use.
        constexpr int kTrailDots = 3;
        for (int i = 1; i <= kTrailDots; ++i)
        {
            const float r = radius * ((float) i / (float) (kTrailDots + 1));
            const auto p = centre.getPointOnCircumference(r, angle);
            g.fillEllipse(p.x - dotSize * 0.5f, p.y - dotSize * 0.5f, dotSize, dotSize);
        }
        g.fillEllipse(centre.x - dotSize * 0.5f, centre.y - dotSize * 0.5f, dotSize, dotSize);
    }

    void drawScreenTexture(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour ink,
                          float dotSize, float pitch)
    {
        g.setColour(ink);
        for (float y = (float) area.getY() + pitch * 0.5f; y < (float) area.getBottom(); y += pitch)
            for (float x = (float) area.getX() + pitch * 0.5f; x < (float) area.getRight(); x += pitch)
                g.fillRect(x, y, dotSize, dotSize);
    }

    void HardwareLcdLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
    {
        const auto& pal = theme->palette();
        auto area = label.getLocalBounds().toFloat().reduced(1.0f);

        // Captions and a Slider's own value textbox render at very different sizes in these
        // popups; scale the dot size off the label's own height so both stay legible rather
        // than sharing one fixed dot size.
        const float dotSize = juce::jlimit(1.1f, 3.4f, area.getHeight() / 9.0f);

        drawText(g, label.getText(), area, dotSize, pal.popupInk, label.getJustificationType());
    }

    void HardwareLcdLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPosProportional, float rotaryStartAngle,
                                                  float rotaryEndAngle, juce::Slider&)
    {
        const auto& pal = theme->palette();
        const juce::Rectangle<float> bounds((float) x, (float) y, (float) width, (float) height);
        const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        drawKnob(g, bounds.reduced(bounds.getWidth() * 0.1f), angle, pal.popupInk);
    }

    void HardwareLcdLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPos, float, float,
                                                  juce::Slider::SliderStyle style, juce::Slider& slider)
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0.0f, 0.0f, style, slider);
            return;
        }

        // A single row of square dots across the track: solid ink up to the value, faint past
        // it -- a dot-matrix "bar graph" readout -- with a 3-dot-tall column as the thumb.
        const auto& pal = theme->palette();
        constexpr float dotSize = 3.0f, pitch = 5.0f;
        const float cy = (float) y + (float) height * 0.5f;
        const float left = (float) x + dotSize, right = (float) (x + width) - dotSize;
        for (float px = left; px <= right; px += pitch)
        {
            g.setColour(pal.popupInk.withAlpha(px <= sliderPos ? 0.9f : 0.22f));
            g.fillRect(px - dotSize * 0.5f, cy - dotSize * 0.5f, dotSize, dotSize);
        }

        const float thumbX = juce::jlimit(left, right, sliderPos);
        g.setColour(pal.popupInk);
        for (int i = -2; i <= 2; ++i)
            g.fillRect(thumbX - dotSize * 0.5f, cy + (float) i * pitch * 0.8f - dotSize * 0.5f, dotSize, dotSize);
    }

    void HardwareLcdLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                      const juce::Colour&, bool isHighlighted, bool isDown)
    {
        const auto& pal = theme->palette();
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        const bool on = button.getToggleState();

        // Solid ink fill when toggled on -- the reference hardware's own highlighted-row look
        // for a selected item (e.g. the inverted "MX INS..." row in the photo) -- a faint hover/
        // press wash otherwise, matching R3WRKLookAndFeel's own pill-button feedback language.
        if (on)
            g.setColour(pal.popupInk.withAlpha(0.85f));
        else
            g.setColour(pal.popupInk.withAlpha(isDown ? 0.30f : (isHighlighted ? 0.14f : 0.0f)));
        g.fillRect(bounds);

        g.setColour(pal.popupInk.withAlpha(0.7f));
        g.drawRect(bounds, 1.0f);
    }

    void HardwareLcdLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool)
    {
        const auto& pal = theme->palette();
        const bool on = button.getToggleState();
        const auto ink = on ? pal.popupBg : pal.popupInk;   // inverted when the ink fill is behind it

        auto area = button.getLocalBounds().toFloat().reduced(2.0f);
        const float dotSize = juce::jlimit(1.2f, 2.6f, area.getHeight() / 9.0f);
        drawText(g, button.getButtonText(), area, dotSize, ink, juce::Justification::centred);
    }

    namespace
    {
        constexpr float kMenuDot = 1.4f;          // menu text dot size
        constexpr float kMenuRowH = 22.0f;
        constexpr float kMenuSepH = 9.0f;
        constexpr float kMenuTickW = 18.0f;       // left gutter for the tick mark
        constexpr float kMenuArrowW = 16.0f;      // right gutter for the submenu arrow
        constexpr float kMenuShortcutGap = 24.0f;
    }

    void HardwareLcdLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
    {
        const auto& pal = theme->palette();
        g.fillAll(pal.popupBg);
        drawScreenTexture(g, { 0, 0, width, height }, pal.popupInk.withAlpha(0.10f));
        g.setColour(pal.popupInk.withAlpha(0.6f));
        g.drawRect(0, 0, width, height, 2);
    }

    void HardwareLcdLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                                           int, int& idealWidth, int& idealHeight)
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = (int) kMenuSepH;
            return;
        }
        // Shortcut width isn't passed here -- reserve room for the longest one used ("⇧⌘Z").
        idealWidth = (int) std::ceil(kMenuTickW + textWidth(text, kMenuDot) + kMenuShortcutGap
                                     + textWidth("XXX", kMenuDot) + kMenuArrowW);
        idealHeight = (int) kMenuRowH;
    }

    void HardwareLcdLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& areaIn,
                                                   bool isSeparator, bool isActive, bool isHighlighted,
                                                   bool isTicked, bool hasSubMenu, const juce::String& text,
                                                   const juce::String& shortcutKeyText,
                                                   const juce::Drawable*, const juce::Colour*)
    {
        const auto& pal = theme->palette();
        auto area = areaIn.toFloat();

        if (isSeparator)
        {
            // A dotted rule, on the same 5px pitch as the background texture.
            g.setColour(pal.popupInk.withAlpha(0.45f));
            const float y = area.getCentreY() - 0.75f;
            for (float x = area.getX() + 8.0f; x < area.getRight() - 8.0f; x += 5.0f)
                g.fillRect(x, y, 1.5f, 1.5f);
            return;
        }

        const bool hot = isHighlighted && isActive;
        if (hot)
        {
            g.setColour(pal.popupInk.withAlpha(0.85f));
            g.fillRect(area.reduced(2.0f, 1.0f));
        }
        const auto ink = hot ? pal.popupBg
                             : pal.popupInk.withAlpha(isActive ? 1.0f : 0.35f);

        auto r = area.reduced(4.0f, 0.0f);
        auto tickArea = r.removeFromLeft(kMenuTickW);
        auto arrowArea = r.removeFromRight(kMenuArrowW);

        if (isTicked)
        {
            const float s = kMenuDot * 4.0f;
            g.setColour(ink);
            g.fillRect(juce::Rectangle<float>(s, s).withCentre(tickArea.getCentre()));
        }

        if (hasSubMenu)
        {
            // A dot-built ">" : three rows stepping out and back.
            g.setColour(ink);
            const float d = kMenuDot, cx = arrowArea.getCentreX() - d, cy = arrowArea.getCentreY();
            for (int i = -3; i <= 3; ++i)
            {
                const float x = cx + (float) (3 - std::abs(i)) * d;
                g.fillRect(x, cy + (float) i * d - d * 0.5f, d * 0.85f, d * 0.85f);
            }
        }

        if (shortcutKeyText.isNotEmpty())
            drawText(g, shortcutKeyText, r, kMenuDot, ink, juce::Justification::centredRight);

        drawText(g, text, r, kMenuDot, ink, juce::Justification::centredLeft);
    }
}
