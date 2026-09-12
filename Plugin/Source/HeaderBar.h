#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"

/**
    The strip above the waveform: an "unsaved" dot + the source file name and the
    "24-bit · 44100 Hz · 2 ch · 12.345 s" readout (bit depth omitted when unknown).
    Modelled on Sieve's audio-editor header.
*/
class HeaderBar : public juce::Component,
                  private juce::Timer,
                  private juce::ChangeListener
{
public:
    explicit HeaderBar(AudioDocument& document);
    ~HeaderBar() override;

    void setSourceName(const juce::String& name);   // "" -> "Untitled"
    void markSaved();                               // call after a successful open / save
    void flashMessage(const juce::String& text);   // brief status in the readout area (~3 s)

    // Clicking the file name reveals it in Finder -- see EditorToolbar::revealCurrentFile(),
    // which this is wired to (PluginEditor). Left to the owner rather than done here directly
    // since HeaderBar only ever sees the display name (setSourceName), not a real juce::File.
    std::function<void()> onNameClicked;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;   // registered on nameLabel too -- see ctor

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;   // theme changed
    void applyTheme();
    juce::String buildReadout() const;
    bool isDirty() const { return document.getBufferVersion() != savedAtVersion; }

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    juce::String sourceName;
    int savedAtVersion = 0;
    bool lastDirty = false;   // last isDirty() seen by the timer, so we only repaint on a change

    juce::String flashText;
    juce::uint32 flashUntil = 0;   // getMillisecondCounter() value; 0 = no flash

    juce::Label nameLabel, readoutLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderBar)
};
