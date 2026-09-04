#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

/** FFT-based spectrogram of the whole document. Recomputed only when the audio content changes. */
class SpectrogramDisplay : public juce::Component,
                           public juce::ChangeListener
{
public:
    explicit SpectrogramDisplay(AudioDocument& doc);
    ~SpectrogramDisplay() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;

private:
    void rebuildImage();
    int64_t xToSample(float x) const;
    float sampleToX(int64_t sample) const;

    AudioDocument& document;
    juce::Image spectrogramImage;
    int lastSeenVersion = -1;
    int64_t dragStartSample = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrogramDisplay)
};
