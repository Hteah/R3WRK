#include "FxRow.h"

FxRow::FxRow(AudioDocument& document, bool standalone)
    : lfoPanel(document, standalone)
{
    addAndMakeVisible(lfoPanel);

    // Placeholder ranges/formatting -- every knob here reads 0-100% until its effect gets real
    // DSP and picks up proper units (Hz, ms, ...). Two per row -- see the class comment for why.
    addSection(0, "DELAY",    "TIME",  "FDBK");
    addSection(1, "REVERB",   "SIZE",  "MIX");
    addSection(1, "GRANULAR", "SIZE",  "DENSITY");

    applyTheme();
    theme->addChangeListener(this);
}

FxRow::~FxRow()
{
    theme->removeChangeListener(this);
}

FxRow::Section& FxRow::addSection(int row, const juce::String& name,
                                  const juce::String& captionA, const juce::String& captionB)
{
    auto* s = sections.add(new Section());
    s->name = name;
    s->row  = row;

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
    auto full = getLocalBounds().reduced(4, 2);
    constexpr int rowGap = 4;
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections

    auto rowTop = full.removeFromTop((full.getHeight() - rowGap) / 2);
    full.removeFromTop(rowGap);
    auto rowBottom = full;

    // Row 0: LFO panel (real) + Delay (placeholder), split evenly like the generic row layout
    // below, but with the LFO panel standing in for what would otherwise be a Section.
    const int halfW = (rowTop.getWidth() - sectionGap) / 2;
    auto lfoArea = rowTop.removeFromLeft(juce::jmax(0, halfW));
    rowTop.removeFromLeft(sectionGap);
    lfoPanel.setBounds(lfoArea);
    if (sections.size() > 0)
        layoutOneSection(*sections[0], rowTop);   // DELAY

    layoutRow(rowBottom, 1);   // REVERB + GRANULAR
    repaint();   // reposition the section dividers for the new layout
}

void FxRow::layoutOneSection(Section& s, juce::Rectangle<int> col)
{
    constexpr int gap = 3;
    constexpr int pillW = 22, pillH = 13;

    s.dividerY = (float) col.getCentreY();
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

void FxRow::layoutRow(juce::Rectangle<int> r, int row)
{
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections

    juce::Array<Section*> rowSections;
    for (auto* s : sections)
        if (s->row == row)
            rowSections.add(s);

    const int n = juce::jmax(1, rowSections.size());
    const int totalGaps  = sectionGap * juce::jmax(0, n - 1);
    // Only two sections share a row (see the class comment), so each gets roughly double the
    // width a four-across layout would allow -- room for more than two knobs once real DSP
    // parameters replace these placeholders.
    const int perSection = juce::jmax(96, (r.getWidth() - totalGaps) / n);

    for (int i = 0; i < rowSections.size(); ++i)
    {
        auto col = r.removeFromLeft(juce::jmin(perSection, r.getWidth()));
        layoutOneSection(*rowSections[i], col);

        if (i < rowSections.size() - 1)
            r.removeFromLeft(sectionGap);
    }
}

void FxRow::paint(juce::Graphics& g)
{
    // Same three-dot divider KnobRow uses between its own sections, centred in the gap between
    // each pair of effect sections that share a row.
    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;
    g.setColour(theme->palette().text.withAlpha(0.4f));

    for (int i = 1; i < sections.size(); ++i)
    {
        const auto* a = sections[i - 1];
        const auto* b = sections[i];
        if (a->row != b->row)
            continue;   // last section of one row, first of the next -- no divider between them
        if (a->outerBounds.isEmpty() || b->outerBounds.isEmpty() || b->outerBounds.getX() <= a->outerBounds.getRight())
            continue;   // not laid out yet

        const float x = (float) (a->outerBounds.getRight() + b->outerBounds.getX()) * 0.5f;
        float y = b->dividerY - (count - 1) * spacing * 0.5f;
        for (int c = 0; c < count; ++c, y += spacing)
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    }
}
