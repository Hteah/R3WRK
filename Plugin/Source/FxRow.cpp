#include "FxRow.h"

FxRow::FxRow(AudioDocument& document, bool standalone)
    : lfoPanel(document, standalone), reverbPanel(document),
      mimeoPanel(document), plexPanel(document)
{
    // lfoPanel/reverbPanel are SHELVED (see class comment) -- constructed and fully wired
    // (their PluginProcessor/AudioDocument side is untouched), just not shown or laid out here.
    // Deliberately no addAndMakeVisible() for either -- bringing them back is exactly those two
    // lines plus their old slot in resized()/paint() below.
    addAndMakeVisible(mimeoPanel);
    addAndMakeVisible(plexPanel);

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
    // mimeoPanel/plexPanel each theme themselves (SharedResourcePointer<ThemeManager>, same
    // ChangeListener pattern) -- the only thing left to refresh here is paint()'s own divider
    // dots, which read the theme directly at paint time.
    repaint();
}

void FxRow::resized()
{
    // One row, two slots: Delay (Mimeophon) | Plexiphon. (LFO/Reverb are shelved -- see class
    // comment -- and take no space here.) Both are real Components.
    auto full = getLocalBounds().reduced(4, 2);
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections
    constexpr int n = 2;

    const int totalGaps = sectionGap * (n - 1);
    const int slotW = juce::jmax(0, (full.getWidth() - totalGaps) / n);

    mimeoPanel.setBounds(full.removeFromLeft(slotW));
    full.removeFromLeft(sectionGap);

    plexPanel.setBounds(full);   // Plexiphon -- takes whatever's left

    repaint();   // reposition the divider dots for the new layout
}

void FxRow::paint(juce::Graphics& g)
{
    // Same three-dot divider KnobRow uses between its own sections, centred in the gap between
    // the two adjacent effect slots (Delay | Plexiphon).
    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;
    g.setColour(theme->palette().text.withAlpha(0.4f));

    const auto a = mimeoPanel.getBounds();
    const auto b = plexPanel.getBounds();
    if (a.isEmpty() || b.isEmpty() || b.getX() <= a.getRight())
        return;   // not laid out yet

    const float x = (float) (a.getRight() + b.getX()) * 0.5f;
    float y = (float) getLocalBounds().getCentreY() - (count - 1) * spacing * 0.5f;
    for (int c = 0; c < count; ++c, y += spacing)
        g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
}
