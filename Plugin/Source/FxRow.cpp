#include "FxRow.h"

FxRow::FxRow(AudioDocument& document, bool standalone)
    : lfoPanel(document, standalone),
      mimeoPanel(document), plexPanel(document), reverbPanel(document)
{
    // lfoPanel is SHELVED (see class comment) -- constructed and fully wired (its
    // PluginProcessor/AudioDocument side is untouched), just not shown or laid out here.
    // Deliberately no addAndMakeVisible() -- bringing it back is exactly that one line plus its
    // old slot in resized()/paint() below.
    addAndMakeVisible(mimeoPanel);
    addAndMakeVisible(plexPanel);
    addAndMakeVisible(reverbPanel);

    applyTheme();
    theme->addChangeListener(this);
}

FxRow::~FxRow()
{
    theme->removeChangeListener(this);
}

void FxRow::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
}

void FxRow::applyTheme()
{
    // mimeoPanel/plexPanel/reverbPanel each theme themselves (SharedResourcePointer
    // <ThemeManager>, same ChangeListener pattern) -- the only thing left to refresh here is
    // paint()'s own divider dots, which read the theme directly at paint time.
    repaint();
}

void FxRow::resized()
{
    // One row, three slots: Delay (Mimeophon) | Plexiphon | Reverb. (LFO is shelved -- see
    // class comment -- and takes no space here.) All three are real Components, each packed to
    // its own CONTENT width -- not stretched across an equal third of the row -- same "packed
    // from the left, sized to content" idiom each panel already uses for its own internal
    // pill+knobs.
    auto full = getLocalBounds().reduced(4, 2);
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections

    // Mimeophon/Plexiphon/Reverb's own resized() all pack pill(32) + gap(3) + 2*(knob 53 +
    // gap 3) + "..."(20) -- kept as a literal here too (not plumbed through as a shared
    // constant) so it can't silently drift from any one panel's own layout. knobW (53) matches
    // KnobRow's own auto-fit upper bound, and pillW (32) matches KnobRow::ModelBadge's own size,
    // so these read the same size as the top row's.
    constexpr int panelW = 32 + 3 + (53 + 3) * 2 + 20;   // 167

    mimeoPanel.setBounds(full.removeFromLeft(juce::jmin(panelW, full.getWidth())));
    full.removeFromLeft(sectionGap);

    plexPanel.setBounds(full.removeFromLeft(juce::jmin(panelW, full.getWidth())));
    full.removeFromLeft(sectionGap);

    reverbPanel.setBounds(full.removeFromLeft(juce::jmin(panelW, full.getWidth())));
    // Whatever's left on the right stays empty, same as KnobRow leaving the drawer toggle's own
    // margin -- not stretched into it.

    repaint();   // reposition the divider dots for the new layout
}

void FxRow::paint(juce::Graphics& g)
{
    // Same three-dot divider KnobRow uses between its own sections, centred in the gap between
    // each pair of adjacent effect slots (Delay | Plexiphon | Reverb).
    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;
    g.setColour(theme->palette().text.withAlpha(0.4f));

    const juce::Array<juce::Rectangle<int>> slots {
        mimeoPanel.getBounds(), plexPanel.getBounds(), reverbPanel.getBounds()
    };

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
