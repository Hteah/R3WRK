#include "HeaderBar.h"

HeaderBar::HeaderBar(AudioDocument& doc) : document(doc)
{
    savedAtVersion = document.getBufferVersion();

    // Pick up wherever the document already knows it's backed by (see AudioDocument::
    // getSourceFilePath()'s comment) rather than always starting "Untitled" -- a live session's
    // document isn't touched by closing/reopening the editor window, and a full session
    // reload restores it via R3WRKAudioProcessor::setStateInformation.
    sourceName = document.getSourceFilePath().isNotEmpty()
                    ? juce::File(document.getSourceFilePath()).getFileName() : juce::String();
    nameLabel.setText(sourceName.isEmpty() ? "Untitled" : sourceName, juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale(1.0f);
    // Click to reveal in Finder (onNameClicked, wired by PluginEditor to EditorToolbar::
    // revealCurrentFile()). addMouseListener rather than a subclass -- Label already consumes
    // its own mouse events (for its double-click-to-edit machinery, unused here since this one
    // is never made editable), so this just observes them alongside that, not instead of it.
    nameLabel.addMouseListener(this, false);
    nameLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    nameLabel.setTooltip("Reveal in Finder");

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

void HeaderBar::mouseUp(const juce::MouseEvent& e)
{
    if (e.eventComponent == &nameLabel && e.mouseWasClicked() && onNameClicked)
        onNameClicked();
}

void HeaderBar::markSaved()
{
    savedAtVersion = document.getBufferVersion();
    repaint();
}

void HeaderBar::flashMessage(const juce::String& text)
{
    flashText = text;
    flashUntil = juce::Time::getMillisecondCounter() + 3000;
    readoutLabel.setColour(juce::Label::textColourId, theme->palette().accent);
    readoutLabel.setText(text, juce::dontSendNotification);
}

juce::String HeaderBar::buildReadout() const
{
    const double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;
    const int    ch = juce::jmax(1, document.getNumChannels());
    const double dur = (double) document.getNumSamples() / sr;
    const juce::String sep = juce::String::fromUTF8(" \xc2\xb7 ");   // " · "

    juce::String s;
    const juce::String bits = document.sourceBitDepthText();
    if (bits.isNotEmpty())
        s << bits << sep;
    s << juce::String((int) sr) << " Hz" << sep << ch << " ch" << sep
      << juce::String(dur, 3) << " s";

    if (document.hasSelection())
    {
        const double sel = (double) (document.getSelectionEnd() - document.getSelectionStart()) / sr;
        s << "  " << sep << "sel " << juce::String(sel, 3) << " s";
    }

    if (document.channelFocus == AudioDocument::ChannelFocus::left)
        s << "  " << sep << "editing L";
    else if (document.channelFocus == AudioDocument::ChannelFocus::right)
        s << "  " << sep << "editing R";

    return s;
}

void HeaderBar::timerCallback()
{
    if (flashUntil != 0)
    {
        if (juce::Time::getMillisecondCounter() < flashUntil)
            return;   // hold the flash message; skip the normal readout update
        flashUntil = 0;
        readoutLabel.setColour(juce::Label::textColourId, theme->palette().textDim);
    }

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
