#pragma once
#include <JuceHeader.h>
#include "Theme.h"
#include <array>

/**
    A small dot-matrix "hardware LCD" rendering kit, styled after classic
    monochrome dot-matrix displays (the reference was an Elektron Octatrack
    screen): every glyph, knob indicator, and background texture is built
    from a coarse grid of small square dots rather than smooth vector shapes.

    Used by LfoEditorPanel (LfoPanel.cpp) and ReverbEditorPanel (ReverbPanel.cpp)
    via HardwareLcdLookAndFeel below, and by R3WRKLookAndFeel::
    drawCallOutBoxBackground (the background texture only, applied to every
    CallOutBox popup app-wide, not just the two dot-matrix ones).

    Deliberately not in namespace r3wrk -- that's reserved for audio-thread-
    safe DSP code; this is plain UI painting, same convention the rest of the
    Source/*.h UI files already follow (no namespace at all, or a small
    dedicated one like this).
*/
namespace lcd
{
    // 5x7 bitmap font -- uppercase A-Z, digits 0-9, and the symbols these
    // popups actually use (space % - + . : /). Callers' text is upper-cased
    // before lookup; a character outside this set renders as blank space.
    using Glyph = std::array<uint8_t, 7>;   // each entry: bits 4..0 = columns left..right
    const Glyph* findGlyph(juce::juce_wchar c) noexcept;

    // Width dot-text of this length/dotSize would need, for layout math without painting.
    float textWidth(const juce::String& text, float dotSize) noexcept;

    // Draws `text` (auto-upper-cased) as a dot-matrix bitmap within `area`, each
    // "on" bit a dotSize-square dot (a touch of corner rounding reads better
    // than a hard square at these sizes). Layout: 5 dots wide + 1 dot gap per
    // character, 7 dots tall, positioned per `justification` inside `area`.
    void drawText(juce::Graphics&, const juce::String& text, juce::Rectangle<float> area,
                 float dotSize, juce::Colour ink,
                 juce::Justification = juce::Justification::centred);

    // A ring of dots for a rotary knob's circle, plus a short radial dot-trail
    // at `angle` (same "0 = up, clockwise" convention as juce::Point::
    // getPointOnCircumference / R3WRKLookAndFeel's own drawRotarySlider) --
    // the "circle + pointer dot" icon language the reference hardware uses
    // for its own rotary-value readouts (PTCH/STRT/LEN/...).
    void drawKnob(juce::Graphics&, juce::Rectangle<float> bounds, float angle, juce::Colour ink);

    // The faint all-over dot grid visible in "blank" areas of a real LCD.
    void drawScreenTexture(juce::Graphics&, juce::Rectangle<int> area, juce::Colour ink,
                          float dotSize = 1.5f, float pitch = 5.0f);

    /**
        LookAndFeel for the two knob-editor popups (LfoEditorPanel,
        ReverbEditorPanel): renders every Label -- including a Slider's own
        built-in value text box, which is a Label internally -- as dot-matrix
        text via drawText(), and every rotary Slider as a dot-matrix knob via
        drawKnob(), instead of R3WRKLookAndFeel's normal smooth rendering.
        Colours come from the live theme (popupBg/popupInk), same
        SharedResourcePointer<ThemeManager> pattern R3WRKLookAndFeel itself
        uses, so it stays in sync with theme changes.
    */
    class HardwareLcdLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        HardwareLcdLookAndFeel() = default;

        void drawLabel(juce::Graphics&, juce::Label&) override;
        void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                              float sliderPosProportional, float rotaryStartAngle,
                              float rotaryEndAngle, juce::Slider&) override;

        // LinearHorizontal sliders: a dot-row bar graph with a dot-column thumb (other linear
        // styles fall back to LookAndFeel_V4).
        void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                              float sliderPos, float minSliderPos, float maxSliderPos,
                              juce::Slider::SliderStyle, juce::Slider&) override;

        // TextButtons (shape/range selectors, enable pills, ...): a plain rectangular dot-
        // outlined "screen button", filled solid ink with inverted (background-coloured) text
        // when toggled on -- the reference hardware's own highlighted-row look for a selected
        // item -- outline-only otherwise.
        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                                  bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        void drawButtonText(juce::Graphics&, juce::TextButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        // PopupMenus (set per menu via PopupMenu::setLookAndFeel -- the Tools menu): popupBg
        // screen with the dot texture, dot-matrix item text + shortcuts, dotted separators,
        // highlighted row inverted (solid ink, background-coloured text).
        void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
        void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                               bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                               bool hasSubMenu, const juce::String& text,
                               const juce::String& shortcutKeyText,
                               const juce::Drawable* icon, const juce::Colour* textColour) override;
        void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                       int standardMenuItemHeight, int& idealWidth,
                                       int& idealHeight) override;
        int getPopupMenuBorderSize() override { return 4; }

    private:
        juce::SharedResourcePointer<ThemeManager> theme;
    };
}
