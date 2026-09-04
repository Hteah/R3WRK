#include "ThemeEditor.h"

ThemeEditor::ThemeEditor()
{
    titleLabel.setText("Theme", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    startLabel.setText("Start from", juce::dontSendNotification);
    startLabel.setFont(juce::FontOptions(11.0f));

    presetBox.setTextWhenNothingSelected("Custom");
    presetBox.onChange = [this]
    {
        const auto name = presetBox.getText();
        if (name.isNotEmpty())
            theme->setPalette(theme->getPreset(name));
    };

    nameField.setTextToShowWhenEmpty(juce::String::fromUTF8("name to save current as\xE2\x80\xA6"),
                                     juce::Colours::grey);
    nameField.setJustification(juce::Justification::centredLeft);

    saveButton.onClick = [this]
    {
        const auto n = nameField.getText().trim();
        if (n.isNotEmpty()) { theme->saveCustom(n, theme->palette()); nameField.clear(); }
    };
    deleteButton.onClick = [this]
    {
        const auto name = presetBox.getText();
        if (theme->isCustom(name))
            theme->deleteCustom(name);
    };
    resetButton.onClick = [this] { theme->setPalette(theme->getPreset("Midnight")); };

    picker.addChangeListener(this);
    pickerDone.onClick = [this] { closePicker(); };

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(startLabel);
    addAndMakeVisible(presetBox);
    addAndMakeVisible(nameField);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(deleteButton);
    addAndMakeVisible(resetButton);
    addChildComponent(picker);
    addChildComponent(pickerDone);

    buildRows();
    rebuildPresetBox();
    refreshFromPalette();

    theme->addChangeListener(this);
    setSize(340, 540);   // 2 rows taller than before: screenText / screenTextDim added
}

ThemeEditor::~ThemeEditor()
{
    theme->removeChangeListener(this);
    picker.removeChangeListener(this);
}

void ThemeEditor::buildRows()
{
    for (int i = 0; i < kNumPaletteFields; ++i)
    {
        auto* r = rows.add(new Row());
        r->fieldIndex = i;

        r->label.setText(kPaletteFields[i].label, juce::dontSendNotification);
        r->label.setFont(juce::FontOptions(12.0f));

        r->hex.setInputRestrictions(8, "0123456789abcdefABCDEF");
        r->hex.setJustification(juce::Justification::centredLeft);
        r->hex.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        r->hex.onReturnKey = [this, i] { commitHex(i); };
        r->hex.onFocusLost = [this, i] { commitHex(i); };

        r->swatch.onClick = [this, i] { openPicker(i); };

        addAndMakeVisible(r->label);
        addAndMakeVisible(r->hex);
        addAndMakeVisible(r->swatch);
    }
}

juce::String ThemeEditor::hexString(juce::Colour c)
{
    const auto s = c.toDisplayString(true);          // 8 lower-case hex digits, "aarrggbb"
    return c.getAlpha() == 255 ? s.substring(2) : s; // hide the alpha when it's opaque
}

void ThemeEditor::commitHex(int fieldIndex)
{
    auto t = rows[fieldIndex]->hex.getText().trim().removeCharacters("#").toLowerCase();
    if (t.length() == 6)
        t = "ff" + t;
    if (t.length() != 8)
    {
        refreshFromPalette();   // not a valid hex code -> put the real value back
        return;
    }

    Palette p = theme->palette();
    p.*(kPaletteFields[fieldIndex].member) = juce::Colour::fromString(t);
    theme->setPalette(p);
}

void ThemeEditor::openPicker(int fieldIndex)
{
    pickerField = fieldIndex;
    picker.setCurrentColour(theme->palette().*(kPaletteFields[fieldIndex].member),
                            juce::dontSendNotification);
    picker.setVisible(true);
    pickerDone.setVisible(true);
    picker.toFront(false);
    pickerDone.toFront(false);
    resized();
}

void ThemeEditor::closePicker()
{
    pickerField = -1;
    picker.setVisible(false);
    pickerDone.setVisible(false);
}

void ThemeEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &picker)
    {
        if (pickerField >= 0 && pickerField < kNumPaletteFields)
        {
            Palette p = theme->palette();
            p.*(kPaletteFields[pickerField].member) = picker.getCurrentColour();
            theme->setPalette(p);
        }
        return;
    }

    // ThemeManager changed (from us or another instance): resync the UI.
    refreshFromPalette();
    rebuildPresetBox();
}

void ThemeEditor::refreshFromPalette()
{
    const auto& p = theme->palette();
    for (auto* r : rows)
    {
        const auto c = p.*(kPaletteFields[r->fieldIndex].member);
        if (! r->hex.hasKeyboardFocus(true))
            r->hex.setText(hexString(c), juce::dontSendNotification);
        r->swatch.setColour(juce::TextButton::buttonColourId, c);
        r->swatch.repaint();
    }

    titleLabel.setColour(juce::Label::textColourId, p.text);
    startLabel.setColour(juce::Label::textColourId, p.textDim);
    for (auto* r : rows)
        r->label.setColour(juce::Label::textColourId, p.textDim);

    deleteButton.setEnabled(theme->isCustom(presetBox.getText()));
    repaint();
}

void ThemeEditor::rebuildPresetBox()
{
    presetBox.clear(juce::dontSendNotification);

    int id = 1;
    for (auto& n : theme->builtInNames())
        presetBox.addItem(n, id++);

    const auto customs = theme->customNames();
    if (! customs.isEmpty())
    {
        presetBox.addSeparator();
        id = 101;
        for (auto& n : customs)
            presetBox.addItem(n, id++);
    }

    int matchId = 0;
    id = 1;
    for (auto& n : theme->builtInNames())
    {
        if (theme->getPreset(n) == theme->palette()) { matchId = id; break; }
        ++id;
    }
    if (matchId == 0)
    {
        id = 101;
        for (auto& n : customs)
        {
            if (theme->getPreset(n) == theme->palette()) { matchId = id; break; }
            ++id;
        }
    }
    presetBox.setSelectedId(matchId, juce::dontSendNotification);
    deleteButton.setEnabled(theme->isCustom(presetBox.getText()));
}

void ThemeEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme->palette().windowBg);
}

void ThemeEditor::resized()
{
    auto r = getLocalBounds().reduced(12);

    titleLabel.setBounds(r.removeFromTop(20));
    r.removeFromTop(6);

    auto sf = r.removeFromTop(24);
    startLabel.setBounds(sf.removeFromLeft(66));
    presetBox.setBounds(sf);
    r.removeFromTop(10);

    const auto rowsArea = r;   // remember for the picker overlay

    for (auto* row : rows)
    {
        auto rr = r.removeFromTop(24);
        row->label.setBounds(rr.removeFromLeft(120));
        row->swatch.setBounds(rr.removeFromRight(26));
        rr.removeFromRight(6);
        row->hex.setBounds(rr.removeFromRight(88));
        r.removeFromTop(2);
    }

    r.removeFromTop(8);
    auto sv = r.removeFromTop(26);
    saveButton.setBounds(sv.removeFromRight(60));
    sv.removeFromRight(6);
    nameField.setBounds(sv);

    r.removeFromTop(6);
    auto bb = r.removeFromTop(26);
    resetButton.setBounds(bb.removeFromRight(64));
    bb.removeFromRight(6);
    deleteButton.setBounds(bb.removeFromRight(64));

    // Colour-picker overlay sits on top of the colour rows.
    if (picker.isVisible())
    {
        auto pk = rowsArea;
        pickerDone.setBounds(pk.removeFromBottom(24).removeFromRight(64));
        pk.removeFromBottom(4);
        picker.setBounds(pk);
    }
}
