#include "HeaderBar.h"

HeaderBar::HeaderBar(AudioDocument& doc) : document(doc)
{
    savedAtVersion = document.getBufferVersion();

    nameLabel.setText("Untitled", juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale(1.0f);

    readoutLabel.setFont(juce::FontOptions(11.0f));

    addAndMakeVisible(nameLabel);
    addAndMakeVisible(readoutLabel);

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(10);
}

HeaderBar::~HeaderBar()
{
    theme->removeChangeListener(this);
}

void HeaderBar::applyTheme()
{
    const auto& pal = theme->palette();
    nameLabel.setColour(juce::Label::textColourId, pal.text);
    readoutLabel.setColour(juce::Label::textColourId, pal.textDim);
}

void HeaderBar::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
    repaint();
}

void HeaderBar::setSourceName(const juce::String& name)
{
    sourceName = name;
    nameLabel.setText(name.isEmpty() ? "Untitled" : name, juce::dontSendNotification);
}

void HeaderBar::markSaved()
{
    savedAtVersion = document.getBufferVersion();
    repaint();
}

juce::String HeaderBar::buildReadout() const
{
    const double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;
    const int    ch = juce::jmax(1, document.getNumChannels());
    const double dur = (double) document.getNumSamples() / sr;
    const juce::String sep = juce::String::fromUTF8(" \xc2\xb7 ");   // " · "

    juce::String s;
    s << juce::String((int) sr) << " Hz" << sep << ch << " ch" << sep
      << juce::String(dur, 3) << " s";

    if (document.hasSelection())
    {
        const double sel = (double) (document.getSelectionEnd() - document.getSelectionStart()) / sr;
        s << "  " << sep << "sel " << juce::String(sel, 3) << " s";
    }
    return s;
}

void HeaderBar::timerCallback()
{
    auto text = buildReadout();
    if (readoutLabel.getText() != text)
        readoutLabel.setText(text, juce::dontSendNotification);

    const bool dirty = isDirty();
    if (dirty != lastDirty) { lastDirty = dirty; repaint(); }
}

void HeaderBar::paint(juce::Graphics& g)
{
    if (isDirty())
    {
        g.setColour(theme->palette().loopMarker);
        g.fillEllipse(4.0f, 6.0f, 8.0f, 8.0f);
    }
}

void HeaderBar::resized()
{
    auto r = getLocalBounds();
    r.removeFromLeft(16);   // room for the dirty dot
    nameLabel.setBounds(r.removeFromTop(r.getHeight() / 2));
    readoutLabel.setBounds(r);
}
