#include "HeaderBar.h"

HeaderBar::HeaderBar(AudioDocument& doc) : document(doc)
{
    savedAtVersion = document.getBufferVersion();

    nameLabel.setText("Untitled", juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale(1.0f);

    readoutLabel.setFont(juce::FontOptions(11.0f));
    readoutLabel.setColour(juce::Label::textColourId, juce::Colours::grey);

    viewButton.setTooltip("Switch between the waveform and spectrogram view");
    viewButton.onClick = [this]
    {
        showingSpectrogram = ! showingSpectrogram;
        viewButton.setButtonText(showingSpectrogram ? "Waveform" : "Spectrogram");
        if (onViewModeChanged) onViewModeChanged(showingSpectrogram);
    };
    zoomFitButton.onClick = [this] { if (onZoomFit) onZoomFit(); };

    addAndMakeVisible(nameLabel);
    addAndMakeVisible(readoutLabel);
    addAndMakeVisible(zoomFitButton);
    addAndMakeVisible(viewButton);

    startTimerHz(10);
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

    static bool lastDirty = false;
    if (isDirty() != lastDirty) { lastDirty = isDirty(); repaint(); }
}

void HeaderBar::paint(juce::Graphics& g)
{
    if (isDirty())
    {
        g.setColour(juce::Colours::orange);
        g.fillEllipse(4.0f, 6.0f, 8.0f, 8.0f);
    }
}

void HeaderBar::resized()
{
    auto r = getLocalBounds();

    auto right = r.removeFromRight(160);
    right = right.withSizeKeepingCentre(right.getWidth(), 24);
    viewButton.setBounds(right.removeFromRight(104));
    right.removeFromRight(4);
    zoomFitButton.setBounds(right.removeFromRight(48));

    r.removeFromLeft(16);   // room for the dirty dot
    nameLabel.setBounds(r.removeFromTop(r.getHeight() / 2));
    readoutLabel.setBounds(r);
}
