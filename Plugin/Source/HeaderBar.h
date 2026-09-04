#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"

/**
    The strip above the waveform: an "unsaved" dot + the source file name and the
    "44100 Hz · 2 ch · 12.345 s" readout. Modelled on Sieve's audio-editor header.
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

    void paint(juce::Graphics&) override;
    void resized() override;

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

    juce::Label nameLabel, readoutLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderBar)
};
