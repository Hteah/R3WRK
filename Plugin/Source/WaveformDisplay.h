#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"
#include "WaveformStretchPreview.h"

/**
    Draws the waveform for the document (one lane per channel), plus a per-lane
    zero-amplitude line, selection, playhead and loop markers.

    Selection (same model as Sieve's audio editor):
      - drag across the waveform to select a range;
      - grab a selection edge (within 8 px) and drag to resize it, anchored to
        the opposite edge; the pointer shows a left/right-resize cursor near an edge;
      - a click (no real drag) clears the selection and moves the playhead there;
      - double-click selects all;
      - drag from inside the selection body to drag the audio out as a WAV file
        (onto an Ableton track, Finder, ...);
      - right-click inside the selection body for a context menu
        (Amplify/Reverse/Stretch·Pitch) -- see onSelectionContextMenu.
    Mouse wheel zooms toward the pointer (or into the selection if there is one);
    horizontal swipe / Shift+wheel pans. ⌘+/⌘- (see zoomIn()/zoomOut(), wired up in
    PluginEditor::keyPressed) zoom without the mouse, behaving like one wheel notch --
    anchored on the selection's midpoint at every zoom level when there is one (not just
    the mouse's incidental position, which wouldn't track the selection once zoomed in
    tighter than it), so repeated keyboard zoom-ins stay centred on the selection instead
    of needing the mouse moved to find it again -- not Control, which turned out to be a
    dead end on macOS twice over (see the comment in PluginEditor::keyPressed). The view
    also auto-refits after an edit that changes the document's length while showing the
    whole thing (see refitViewIfContentChanged()).

    While the Scrub tool is selected (document.scrubModeEnabled, toggled by
    EditorToolbar's Scrub button), every mouse gesture here is repurposed: drag across the
    waveform to play forward or backward at a rate matching how fast you drag, like moving
    tape past a playback head by hand -- see mouseDown()/mouseDrag()/mouseUp() and
    PluginProcessor::renderScrub().

    While Speed/Pitch/Stretch are non-identity, the drawn waveform *shape* comes from a real
    offline stretch computed in the background (see WaveformStretchPreview / stretchPreview)
    rather than the stored audio simply rescaled -- so it actually shows how the audio's
    detail reshapes at the current knob settings (transients smear at extreme ratios, etc.),
    not just how long it'll be.
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

    // Sample <-> pixel mapping, accounting for the live Speed/Pitch/Stretch knobs (see
    // AudioDocument::getTimeScale()) -- so the waveform visually stretches/compresses to
    // show how long the clip will actually play, sampler-style. Public so TimeRuler can
    // place its playhead marker through the same mapping.
    int64_t xToSample(float x) const;
    float sampleToX(int64_t sample) const;

    /// Called after a real drag-selection or an edge-resize finishes (selection already updated).
    std::function<void()> onSelectionCommitted;

    /// Right-click inside the selection body (not near an edge) -- e.g. show a context menu
    /// (Amplify/Reverse/Stretch·Pitch) at the given screen position.
    std::function<void(juce::Point<int> screenPosition)> onSelectionContextMenu;

    /// Which drag a press begins, given the press x and the selection-edge x's (pixels).
    /// Pure + static so it can be unit-tested. `newSelection` when neither edge is within
    /// `tolerance`, otherwise the nearer edge.
    enum class EdgeHit { newSelection, resizeStart, resizeEnd };
    static EdgeHit hitEdge(float pressX, float startX, float endX, float tolerance);

private:
    void timerCallback() override;
    bool refitViewIfContentChanged();   // see .cpp -- keeps "show everything" showing everything
    void paintRecordingScope(juce::Graphics&);
    void paintSelectionPreview(juce::Graphics&);   // live Amplify/Stretch preview overlay, see .cpp
    void beginSelectionDragExport();   // native file drag of the selection to Ableton / Finder
    void rebuildPeakCache();           // scan the buffer once per content change (holds the lock briefly)
    void rebuildWaveformPath();        // build the display path from the cache (no lock) per view change
    void zoomToward(double spanFactor, float pointerX);   // wheel zoom (Sieve model)
    void panByPixels(float dxPixels);
    float currentMouseX() const;   // mouse position in this component's coords, clamped on-screen
    float keyboardZoomAnchorX() const;   // the selection's midpoint if there is one, else currentMouseX()
    int64_t maxViewSpan() const;   // largest sensible viewEnd-viewStart, for the current
                                   // sample count and timeScale -- see .cpp
    int64_t effectiveSpanFor(int64_t rawTotal, double timeScale) const;   // maxViewSpan(), but
                                                                          // for a given (e.g. an
                                                                          // *old*) total/timeScale

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    int64_t viewStart = 0, viewEnd = 0;
    std::vector<juce::Path> channelPaths;
    int lastBufferVersion = -1;      // rebuild the paths when the audio content changes
    int64_t lastKnownTotal = 0;      // getNumSamples() as of lastBufferVersion -- see
                                      // refitIfWasShowingWholeDocument()
    int lastPathWidth = 0, lastPathHeight = 0;
    double lastTimeScale = 1.0;      // rebuild the path when Speed/Pitch/Stretch move the knobs

    // Peak cache: one min/max per `peakBinSize` source samples, per channel. Rebuilt only
    // when the audio content changes -- so zoom/pan build the path from this with no lock,
    // instead of scanning document.getBuffer() under getLock() (which was starving
    // processBlock's try-lock and clicking playback when zooming in a DAW).
    static constexpr int peakBinSize = 64;
    std::vector<std::vector<float>> chPeakMin, chPeakMax;
    int peakVersion = -1;
    int64_t peakTotalSamples = 0;

    WaveformStretchPreview stretchPreview { document };   // background real-stretch preview

    // A scroll/trackpad gesture sends a rapid burst of small wheel events; re-reading the
    // pointer's exact x on every single one means incidental mouse jitter during the gesture
    // (nobody's hand is perfectly still) nudges the zoom anchor a little each time, drifting
    // away from wherever you actually meant to zoom in on by the time a many-notch gesture is
    // done. Locking the anchor x to wherever the gesture *started* and reusing it for the
    // whole burst -- see mouseWheelMove() -- fixes that; a pause longer than
    // wheelGestureGapMs starts a fresh gesture (and a fresh anchor) on the next notch.
    static constexpr uint32_t wheelGestureGapMs = 400;
    uint32_t lastWheelEventMs = 0;
    float wheelGestureAnchorX = 0.0f;

    enum class DragKind { none, newSelection, resizeStart, resizeEnd, dragOut };
    DragKind dragKind = DragKind::none;
    int64_t dragAnchor = 0;          // fixed frame: press frame (new) or the opposite edge (resize)
    bool dragOutStarted = false;     // the native file drag for this gesture has been kicked off
    static constexpr float edgeTolerancePx = 8.0f;

    // Scrub tool (document.scrubModeEnabled) -- entirely separate from the DragKind state
    // above; a scrub drag never touches the selection. mouseDrag re-derives a velocity
    // (raw samples/sec, signed) from how far and how fast the sample under the pointer has
    // moved since the last event, and writes it to document.scrubVelocity for the audio
    // thread (see PluginProcessor::renderScrub) to integrate -- so the pitch rises and
    // falls with drag speed, exactly like moving real tape past a playback head by hand.
    int64_t scrubLastSample = 0;
    double scrubLastTimeMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDisplay)
};
