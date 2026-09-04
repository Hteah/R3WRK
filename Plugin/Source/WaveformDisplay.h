#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"

/**
    Draws the waveform for the document (one lane per channel), plus a per-lane
    zero-amplitude line, selection, playhead and loop markers.

    Selection (same model as Sieve's audio editor):
      - drag across the waveform to select a range;
      - grab a selection edge (within 8 px) and drag to resize it, anchored to
        the opposite edge; the pointer shows a left/right-resize cursor near an edge;
      - a click (no real drag) clears the selection and moves the playhead there;
      - double-click selects all.
    Mouse wheel zooms toward the pointer (or into the selection if there is one);
    horizontal swipe / Shift+wheel pans; the header's Fit button resets the view.
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
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void zoomToFit();
    void zoomIn();
    void zoomOut();

    int64_t getViewStart() const { return viewStart; }
    int64_t getViewEnd() const { return viewEnd; }

    /// Called after a real drag-selection or an edge-resize finishes (selection already updated).
    std::function<void()> onSelectionCommitted;

    /// Which drag a press begins, given the press x and the selection-edge x's (pixels).
    /// Pure + static so it can be unit-tested. `newSelection` when neither edge is within
    /// `tolerance`, otherwise the nearer edge.
    enum class EdgeHit { newSelection, resizeStart, resizeEnd };
    static EdgeHit hitEdge(float pressX, float startX, float endX, float tolerance);

private:
    void timerCallback() override;
    void paintRecordingScope(juce::Graphics&);
    void rebuildWaveformPath();
    void zoomBy(double factor, int64_t centerSample);
    void zoomToward(double spanFactor, float pointerX);   // wheel zoom (Sieve model)
    void panByPixels(float dxPixels);
    int64_t xToSample(float x) const;
    float sampleToX(int64_t sample) const;

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    int64_t viewStart = 0, viewEnd = 0;
    std::vector<juce::Path> channelPaths;
    int lastBufferVersion = -1;      // rebuild the paths when the audio content changes
    int lastPathWidth = 0, lastPathHeight = 0;

    enum class DragKind { none, newSelection, resizeStart, resizeEnd };
    DragKind dragKind = DragKind::none;
    int64_t dragAnchor = 0;          // fixed frame: press frame (new) or the opposite edge (resize)
    static constexpr float edgeTolerancePx = 8.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDisplay)
};
