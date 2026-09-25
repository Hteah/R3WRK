#pragma once
#include <JuceHeader.h>
#include "Theme.h"

/**
    The theme panel, shown from Tools ▾ → "Theme…" in a CallOutBox.

      - "Start from" combo: built-in presets + your saved ones.
      - one hex row per palette colour (type a hex code, or click the swatch to
        open the colour picker in place) — changes apply live everywhere.
      - name field + Save to store the current palette as a custom preset;
        Delete removes the selected custom preset; Reset returns to Midnight.
      - Copy / Paste: the theme as a line of text, to move it to Sieve (or back) without
        saving. Saved themes live in the shared themes folder, so Sieve lists them too.

    Everything lives inside this one component (the picker is an in-panel overlay,
    not a nested call-out) so the enclosing CallOutBox never dismisses itself.
*/
class ThemeEditor : public juce::Component,
                    private juce::ChangeListener
{
public:
    ThemeEditor();
    ~ThemeEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::Label       label;
        juce::TextEditor  hex;
        juce::TextButton  swatch;
        int               fieldIndex = 0;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void buildRows();
    void refreshFromPalette();
    void rebuildPresetBox();
    void commitHex(int fieldIndex);
    void openPicker(int fieldIndex);
    void closePicker();
    static juce::String hexString(juce::Colour);

    juce::SharedResourcePointer<ThemeManager> theme;

    juce::Label      titleLabel;
    juce::Label      startLabel;
    juce::ComboBox   presetBox;
    juce::ToggleButton shadedToggle { "Shaded panel" };
    juce::Label      darkenLabel { {}, "Edge darken" };
    juce::Slider     darkenSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label      alphaLabel  { {}, "Edge opacity" };
    juce::Slider     alphaSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::OwnedArray<Row> rows;

    juce::TextEditor nameField;
    juce::TextButton saveButton   { "Save" };
    juce::TextButton deleteButton { "Delete" };
    juce::TextButton resetButton  { "Reset" };
    juce::TextButton copyButton   { "Copy" };    // theme as text, for pasting into Sieve
    juce::TextButton pasteButton  { "Paste" };

    juce::ColourSelector picker;
    juce::TextButton     pickerDone { "Done" };
    int pickerField = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeEditor)
};
