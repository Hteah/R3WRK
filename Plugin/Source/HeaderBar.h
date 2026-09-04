#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

/**
    The strip above the waveform: an "unsaved" dot + the source file name, the
    "44100 Hz · 2 ch · 12.345 s" readout, and the Fit / view-toggle buttons.
    Modelled on Sieve's audio-editor header.
*/
class HeaderBar : public juce::Component, private juce::Timer
{
public:
    explicit HeaderBar(AudioDocument& document);
    ~HeaderBar() override = default;

    std::function<void()> onZoomFit;
    std::function<void(bool showSpectrogram)> onViewModeChanged;

    void setSourceName(const juce::String& name);   // "" -> "Untitled"
    void markSaved();                               // call after a successful open / save

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::String buildReadout() const;
    bool isDirty() const { return document.getBufferVersion() != savedAtVersion; }

    AudioDocument& document;
    juce::String sourceName;
    int savedAtVersion = 0;
    bool showingSpectrogram = false;
    bool lastDirty = false;   // last isDirty() seen by the timer, so we only repaint on a change

    juce::Label nameLabel, readoutLabel;
    juce::TextButton zoomFitButton { "Fit" };
    juce::TextButton viewButton    { "Spectrogram" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderBar)
};
