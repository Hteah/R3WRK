#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

/**
    Draws the waveform for the document (one lane per channel), plus selection,
    playhead, loop markers and chop-grid lines. Click-drag selects a range;
    single click moves the playhead; double-click selects all.
    Mouse wheel scrolls; Ctrl/Cmd + wheel zooms.
*/
class WaveformDisplay : public juce::Component,
                         public juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit WaveformDisplay(AudioDocument& doc);
    ~WaveformDisplay() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void zoomToFit();
    void zoomIn();
    void zoomOut();

    int64_t getViewStart() const { return viewStart; }
    int64_t getViewEnd() const { return viewEnd; }

private:
    void timerCallback() override;
    void rebuildWaveformPath();
    void zoomBy(double factor, int64_t centerSample);
    int64_t xToSample(float x) const;
    float sampleToX(int64_t sample) const;

    AudioDocument& document;
    int64_t viewStart = 0, viewEnd = 0;
    std::vector<juce::Path> channelPaths;
    int lastBufferVersion = -1;      // rebuild the paths when the audio content changes
    int lastPathWidth = 0, lastPathHeight = 0;

    int64_t dragStartSample = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDisplay)
};
