#pragma once
#include <JuceHeader.h>

/**
    The editable colour palette for the whole editor, plus a process-wide manager
    that persists it to a settings file shared by every plugin instance and the
    standalone (the JUCE-plugin equivalent of RCRDR's @AppStorage theme).

    Every painted component holds a `juce::SharedResourcePointer<ThemeManager>`,
    reads `theme->palette()` when it paints, and listens to the manager so it
    repaints the moment a colour changes.
*/
struct Palette
{
    juce::Colour windowBg     { 0xff14161a };
    juce::Colour panelBg      { 0xff17191e };
    juce::Colour popupBg      { 0xffada56a };   // the CallOutBox "bubble" background (LFO/Reverb
                                                 // editors, Theme panel itself, ...) -- see
                                                 // R3WRKLookAndFeel::drawCallOutBoxBackground.
                                                 // Olive/amber by default -- a monochrome-LCD
                                                 // look (see popupInk, DotMatrixLCD.h).
    juce::Colour popupInk     { 0xff3f3416 };   // the "pixel" colour on that screen -- dot-
                                                 // matrix text/knobs (DotMatrixLCD.h) and the
                                                 // popup's own faint background texture/border.
    juce::Colour waveform     { 0xff5ec2ff };
    juce::Colour accent       { 0xff5ec2ff };   // selection brackets, knob fills
    juce::Colour zeroLine     { 0x29ffffff };
    juce::Colour gridLine     { 0x80000000 };   // lane dividers, ruler hairline
    juce::Colour playhead     { 0xffff3b30 };
    juce::Colour loopMarker   { 0xffffa500 };   // also the "unsaved" dot
    juce::Colour recordButton { 0xff8b0000 };
    juce::Colour text         { 0xffe0e0e0 };
    juce::Colour textDim      { 0xff888888 };

    // Separate from text/textDim because they paint on a different surface: text/textDim
    // sit on windowBg (the header, transport, knobs -- components with no fill of their
    // own), while screenText/screenTextDim sit on panelBg (WaveformDisplay, TimeRuler --
    // the "screen"). A theme where those two backgrounds are opposite in lightness (e.g. a
    // light chrome around a dark waveform) needs two different text colours to stay legible
    // on both; every theme before that one just set these equal to text/textDim.
    juce::Colour screenText    { 0xffe0e0e0 };
    juce::Colour screenTextDim { 0xff888888 };

    // When true, the editor's own background (PluginEditor::paint) is painted as an inset
    // shadow bled in from each of the four edges toward the waveform display, instead of a
    // flat windowBg fill -- a "panel body" look, toggleable per-theme from ThemeEditor
    // (works for any theme, light or dark, since it's computed from that theme's own
    // windowBg rather than fixed colours). Not a Colour, so it's serialised separately from
    // kPaletteFields.
    bool shadedPanel = false;

    // Edge-shading intensity, editable live from ThemeEditor's sliders. edgeShadeDarken
    // (0-1): how far PluginEditor::paint's edge-shadow colour is darkened from windowBg
    // (juce::Colour::darker's amount). edgeShadeAlpha (0-1): that shadow's peak opacity,
    // right at the window edge. Both no-ops while shadedPanel is off.
    float edgeShadeDarken = 0.85f;
    float edgeShadeAlpha  = 0.42f;

    juce::String toString() const;                       // "key:aarrggbb;key:aarrggbb;..."
    static Palette fromString(const juce::String&);      // unknown keys ignored, missing keep default

    bool operator== (const Palette&) const;
    bool operator!= (const Palette& o) const { return ! (*this == o); }
};

// One entry per editable colour, in display order. Drives both the editor UI and (de)serialisation.
struct PaletteField
{
    const char* key;
    const char* label;
    juce::Colour Palette::* member;
};
extern const PaletteField kPaletteFields[];
extern const int kNumPaletteFields;

class ThemeManager : public juce::ChangeBroadcaster,
                     private juce::Timer
{
public:
    ThemeManager();
    ~ThemeManager() override;

    const Palette& palette() const noexcept { return active; }
    void setPalette (const Palette&);        // apply live + debounced save + broadcast

    juce::StringArray builtInNames() const;  // "Start from..." presets baked into the code
    juce::StringArray customNames() const;   // user-saved presets from the settings file
    bool isCustom (const juce::String& name) const;
    Palette getPreset (const juce::String& name) const;   // built-in or custom; Midnight if unknown

    void saveCustom (const juce::String& name, const Palette&);
    void deleteCustom (const juce::String& name);

private:
    void timerCallback() override;           // debounced settings-file write
    void load();
    juce::PropertiesFile& props();

    Palette active;
    std::unique_ptr<juce::PropertiesFile> propsFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeManager)
};
