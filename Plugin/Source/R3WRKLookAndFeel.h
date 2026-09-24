#pragma once
#include <JuceHeader.h>
#include "Theme.h"

// Explicit-typeface font helpers -- see their own comments in R3WRKLookAndFeel.cpp. `bold`
// defaults to true since every current caller wants it; pass false for the rare regular-weight
// case.
juce::Font spaceMonoFont(float height, bool bold = true);
juce::Font systemUIFont(float height, bool bold = true);

/**
    The shared custom look: rotary knobs (a flat disc, a thin outline, a single pointer
    line -- no value-arc, Eurorack/VCV-module-inspired), pill-shaped buttons (fully
    rounded, filled when "on" or accent-primary, outlined/transparent otherwise), and a
    handful of drawn transport icons in place of button text (play triangle, stop square,
    loop's two-arrow circle, play-from-start's bar+triangle, Tools' gear, Scrub's
    notched reel hub, Reverse's leftward arrow, Clear's X, Auto-Record's gauge,
    Slice's marker flag, Follow's playhead-on-a-ruler) -- see drawButtonText().
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

    // EXPERIMENT (branch experiment/space-mono-font): swaps every font request -- default
    // sans, bold, and the explicit getDefaultMonospacedFontName() ones (TimeRuler, hex
    // field, etc.) alike -- for the embedded Space Mono typeface, app-wide, without
    // touching each of the dozens of individual setFont() call sites. Space Mono is
    // itself monospaced, so the monospace call sites read the same as before, just in the
    // new face.
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    // Bumps the rotary value readout below each knob to 16pt (LookAndFeel_V4's own default
    // reads noticeably smaller/thinner). KnobRow used to set this only for its own knobs via a
    // private subclass; moved up to the shared default here so every other rotary knob using
    // this look -- the FX drawer's compact panels (Mimeophon/Plexiphon/Reverb) included --
    // matches KnobRow's readout size too, instead of falling back to the JUCE default.
    juce::Label* createSliderTextBox(juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // A button whose text is one of the "icon:..." markers below (see EditorToolbar) gets a
    // drawn vector icon instead of literal text; anything else falls back to the normal
    // JUCE text rendering, so this is safe to use on every button, not just the transport.
    static constexpr const char* iconPlay          = "icon:play";
    static constexpr const char* iconStop          = "icon:stop";
    static constexpr const char* iconLoop          = "icon:loop";
    static constexpr const char* iconInfinity      = "icon:infinity";      // ping-pong loop (a figure-eight)
    static constexpr const char* iconLoopReverse   = "icon:loopReverse";   // reverse loop (mirror of iconLoop)
    static constexpr const char* iconPlayFromStart = "icon:playFromStart";   // a bar + the play triangle
    static constexpr const char* iconTools         = "icon:tools";          // a gear/cog
    static constexpr const char* iconScrub         = "icon:scrub";          // a notched reel hub
    static constexpr const char* iconReverse       = "icon:reverse";        // an arrow pointing left
    static constexpr const char* iconClear         = "icon:clear";          // an X
    static constexpr const char* iconAutoRecord    = "icon:autoRecord";     // a gauge (ring + arc + needle)
    static constexpr const char* iconSlice         = "icon:slice";          // a marker flag (pole + pennant)
    static constexpr const char* iconFollow        = "icon:follow";         // playhead on a ruler + follow chevrons
    static constexpr const char* iconDesktopRec    = "icon:desktopRec";     // a monitor/display outline + record dot
    static constexpr const char* iconFloatTop      = "icon:floatTop";       // two rings, arrows pointing right/up (float on top)
    static constexpr const char* iconCaptureOut    = "icon:captureOut";     // record dot + down arrow (capture the output to a file)
    static constexpr const char* iconBlackBox      = "icon:blackBox";       // rounded box + record dot (VST/AU always-on background capture)
    static constexpr const char* iconOrbit         = "icon:orbit";          // the FX drawer toggle -- concentric dashed orbit rings around a centre dot, from the r3wrk Component Library reference sheet
    static constexpr const char* iconMore          = "icon:more";           // a single small filled circle -- the popup-editor "more" buttons (Mimeophon/Plexiphon/Reverb), replacing the old "..." text

    void drawButtonText(juce::Graphics&, juce::TextButton&,
                       bool isMouseOverButton, bool isButtonDown) override;

    // The CallOutBox "bubble" (LFO/Reverb editors, the Theme panel itself, AutoRecordThreshold-
    // Panel, ...) otherwise inherits LookAndFeel_V4's default -- JUCE's own generic colour
    // scheme, not this app's theme -- so it never matched a theme's actual palette.
    void drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics&,
                                  const juce::Path&, juce::Image&) override;

private:
    juce::SharedResourcePointer<ThemeManager> theme;
};

/**
    Same drawn icons again, but no box at all -- not even a hover/press wash. Nothing but the
    icon itself, ever, in any state; on/off (open/closed) reads purely through the icon's own
    ink colour (see drawButtonText: textColourOnId vs. textColourOffId), the same way every
    other static icon in this file already communicates toggle state. For KnobRow's FX drawer
    toggle (the orbit icon), per the user's request to remove the square around it entirely.
*/
class R3WRKIconOnlyLookAndFeel : public R3WRKLookAndFeel
{
public:
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool, bool) override
    {
        // Deliberately empty -- see class comment.
    }
};
