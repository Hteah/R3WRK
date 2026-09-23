#include "FxRow.h"

FxRow::FxRow(AudioDocument& document, bool standalone)
    : lfoPanel(document, standalone), reverbPanel(document), plexPanel(document)
{
    addAndMakeVisible(lfoPanel);
    addAndMakeVisible(reverbPanel);
    addAndMakeVisible(plexPanel);

    // Placeholder ranges/formatting -- every knob here reads 0-100% until its effect gets real
    // DSP and picks up proper units (Hz, ms, ...).
    addSection("DELAY", "TIME", "FDBK");

    applyTheme();
    theme->addChangeListener(this);
}

FxRow::~FxRow()
{
    theme->removeChangeListener(this);
}

FxRow::Section& FxRow::addSection(const juce::String& name,
                                  const juce::String& captionA, const juce::String& captionB)
{
    auto* s = sections.add(new Section());
    s->name = name;

    s->pill.text = name.substring(0, 4);
    s->pill.onClick = [s]
    {
        s->pill.on = ! s->pill.on;
        s->pill.repaint();
    };
    addAndMakeVisible(s->pill);

    auto setUpKnob = [this](Knob& k, const juce::String& caption)
    {
        k.caption.setText(caption, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(14.0f));
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.updateText();
        addAndMakeVisible(k.caption);
        addAndMakeVisible(k.slider);
    };
    setUpKnob(s->knobA, captionA);
    setUpKnob(s->knobB, captionB);

    return *s;
}

void FxRow::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
}

void FxRow::applyTheme()
{
    const auto& pal = theme->palette();
    for (auto* s : sections)
    {
        s->pill.fill   = pal.accent;
        s->pill.ink    = pal.windowBg;
        s->pill.border = pal.textDim;
        s->pill.repaint();

        for (auto* k : { &s->knobA, &s->knobB })
        {
            k->caption.setColour(juce::Label::textColourId, pal.textDim);
            k->caption.repaint();
            k->slider.repaint();
        }
    }
}

void FxRow::EnablePill::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    constexpr float rad = 3.0f;
    if (on)
        g.setColour(fill);
    else
        g.setColour(fill.withAlpha(hovered ? 0.22f : 0.0f));
    g.fillRoundedRectangle(r, rad);
    g.setColour(border.withAlpha(on || hovered ? 0.95f : 0.55f));
    g.drawRoundedRectangle(r, rad, 1.0f);
    g.setColour(on ? ink : border);
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    g.drawText(text, getLocalBounds(), juce::Justification::centred);
}

void FxRow::resized()
{
    // One row, four slots: LFO | Delay | Reverb | Plexiphon. LFO/Reverb/Plexiphon are real
    // Components standing in for what would otherwise be a Section; Delay still is one.
    auto full = getLocalBounds().reduced(4, 2);
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections
    constexpr int n = 4;

    const int totalGaps = sectionGap * (n - 1);
    const int slotW = juce::jmax(0, (full.getWidth() - totalGaps) / n);

    lfoPanel.setBounds(full.removeFromLeft(slotW));
    full.removeFromLeft(sectionGap);

    if (sections.size() > 0)
        layoutOneSection(*sections[0], full.removeFromLeft(slotW));   // DELAY
    else
        full.removeFromLeft(slotW);
    full.removeFromLeft(sectionGap);

    reverbPanel.setBounds(full.removeFromLeft(slotW));
    full.removeFromLeft(sectionGap);

    plexPanel.setBounds(full);   // Plexiphon -- takes whatever's left

    repaint();   // reposition the section dividers for the new layout
}

void FxRow::layoutOneSection(Section& s, juce::Rectangle<int> col)
{
    constexpr int gap = 3;
    constexpr int pillW = 22, pillH = 13;

    s.outerBounds = col;

    auto pillArea = col.removeFromLeft(pillW);
    s.pill.setBounds(pillArea.withSizeKeepingCentre(pillW, pillH));
    col.removeFromLeft(gap);

    const int knobW = juce::jlimit(40, 60, (col.getWidth() - gap) / 2);
    for (auto* k : { &s.knobA, &s.knobB })
    {
        auto kcol = col.removeFromLeft(knobW);
        k->caption.setBounds(kcol.removeFromTop(15));
        k->slider.setBounds(kcol);
        col.removeFromLeft(gap);
    }
}

void FxRow::paint(juce::Graphics& g)
{
    // Same three-dot divider KnobRow uses between its own sections, centred in the gap between
    // each pair of adjacent effect slots (LFO | Delay | Reverb | Plexiphon) -- built from a
    // uniform list of all four slots' bounds, not just the one placeholder Section's, since
    // LFO/Reverb/Plexiphon (real Components, not Sections) sit among them in the row.
    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;
    g.setColour(theme->palette().text.withAlpha(0.4f));

    juce::Array<juce::Rectangle<int>> slots;
    slots.add(lfoPanel.getBounds());
    if (sections.size() > 0) slots.add(sections[0]->outerBounds);   // DELAY
    slots.add(reverbPanel.getBounds());
    slots.add(plexPanel.getBounds());

    for (int i = 1; i < slots.size(); ++i)
    {
        const auto a = slots[i - 1];
        const auto b = slots[i];
        if (a.isEmpty() || b.isEmpty() || b.getX() <= a.getRight())
            continue;   // not laid out yet

        const float x = (float) (a.getRight() + b.getX()) * 0.5f;
        float y = (float) getLocalBounds().getCentreY() - (count - 1) * spacing * 0.5f;
        for (int c = 0; c < count; ++c, y += spacing)
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    }
}
