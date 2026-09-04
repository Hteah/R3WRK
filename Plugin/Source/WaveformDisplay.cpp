#include "WaveformDisplay.h"
#include <cmath>

WaveformDisplay::WaveformDisplay(AudioDocument& doc) : document(doc)
{
    document.changeBroadcaster.addChangeListener(this);
    viewEnd = document.getNumSamples();
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

WaveformDisplay::~WaveformDisplay()
{
    document.changeBroadcaster.removeChangeListener(this);
}

void WaveformDisplay::timerCallback()
{
    // Rebuild whenever the audio content changed, or the view range is stale / degenerate,
    // or the component was resized before the buffer existed. Cheaper than trusting only the
    // async ChangeBroadcaster, and self-heals any ordering race on first load.
    const int version = document.getBufferVersion();
    const int64_t total = document.getNumSamples();
    const bool viewBad = viewEnd <= viewStart || viewEnd > total || viewStart >= juce::jmax((int64_t) 1, total);

    if (version != lastBufferVersion || viewBad
        || getWidth() != lastPathWidth || getHeight() != lastPathHeight)
    {
        lastBufferVersion = version;
        if (viewBad)
        {
            viewStart = 0;
            viewEnd = juce::jmax((int64_t) 1, total);
        }
        rebuildWaveformPath();
    }
    repaint();
}

int64_t WaveformDisplay::xToSample(float x) const
{
    int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    double frac = (double) x / (double) juce::jmax(1, getWidth());
    return juce::jlimit((int64_t) 0, document.getNumSamples(), viewStart + (int64_t) (frac * (double) rangeLen));
}

float WaveformDisplay::sampleToX(int64_t sample) const
{
    int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    return (float) getWidth() * (float) ((double) (sample - viewStart) / (double) rangeLen);
}

void WaveformDisplay::zoomToFit()
{
    viewStart = 0;
    viewEnd = juce::jmax((int64_t) 1, document.getNumSamples());
    rebuildWaveformPath();
    repaint();
}

void WaveformDisplay::zoomBy(double factor, int64_t centerSample)
{
    int64_t len = juce::jmax((int64_t) 1, viewEnd - viewStart);
    int64_t newLen = (int64_t) juce::jlimit(200.0, (double) juce::jmax((int64_t) 200, document.getNumSamples()),
                                             (double) len * factor);
    double centerFrac = len > 0 ? (double) (centerSample - viewStart) / (double) len : 0.5;
    int64_t newStart = centerSample - (int64_t) (centerFrac * (double) newLen);
    int64_t newEnd = newStart + newLen;

    if (newStart < 0) { newEnd -= newStart; newStart = 0; }
    if (newEnd > document.getNumSamples())
    {
        int64_t over = newEnd - document.getNumSamples();
        newEnd = document.getNumSamples();
        newStart = juce::jmax((int64_t) 0, newStart - over);
    }

    viewStart = newStart;
    viewEnd = newEnd;
    rebuildWaveformPath();
    repaint();
}

void WaveformDisplay::zoomIn()  { zoomBy(0.5, (viewStart + viewEnd) / 2); }
void WaveformDisplay::zoomOut() { zoomBy(2.0, (viewStart + viewEnd) / 2); }

// Sieve's editor zoom: `spanFactor` multiplies the visible span (<1 = zoom in). Keeps the
// sample under `pointerX` fixed — but while zooming in with a selection that's still narrower
// than the view, it frames the selection instead so the wheel pulls you into it.
void WaveformDisplay::zoomToward(double spanFactor, float pointerX)
{
    const int64_t total = document.getNumSamples();
    if (total <= 0)
        return;

    spanFactor = juce::jlimit(0.5, 2.0, spanFactor);
    const int64_t curLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    int64_t newLen = (int64_t) juce::jlimit(16.0, (double) total, (double) curLen * spanFactor);

    if (spanFactor < 1.0 && document.hasSelection())
    {
        const int64_t selLen = document.getSelectionEnd() - document.getSelectionStart();
        if (selLen > 0 && curLen > selLen)
        {
            newLen = juce::jmax((int64_t) 16, juce::jmin(newLen, selLen + selLen / 5));   // stop ~1.2× the selection
            const int64_t mid = (document.getSelectionStart() + document.getSelectionEnd()) / 2;
            int64_t newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, total - newLen), mid - newLen / 2);
            viewStart = newStart;
            viewEnd = newStart + newLen;
            rebuildWaveformPath();
            repaint();
            return;
        }
    }

    const double frac = juce::jlimit(0.0, 1.0, (double) pointerX / (double) juce::jmax(1, getWidth()));
    const int64_t anchor = viewStart + (int64_t) (frac * (double) curLen);
    int64_t newStart = anchor - (int64_t) (frac * (double) newLen);
    newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, total - newLen), newStart);
    viewStart = newStart;
    viewEnd = newStart + newLen;
    rebuildWaveformPath();
    repaint();
}

void WaveformDisplay::panByPixels(float dxPixels)
{
    const int64_t total = document.getNumSamples();
    const int64_t len = viewEnd - viewStart;
    if (len <= 0 || len >= total)
        return;

    const double framesPerPixel = (double) len / (double) juce::jmax(1, getWidth());
    int64_t newStart = viewStart - (int64_t) (dxPixels * framesPerPixel);
    newStart = juce::jlimit((int64_t) 0, total - len, newStart);
    viewStart = newStart;
    viewEnd = newStart + len;
    rebuildWaveformPath();
    repaint();
}

void WaveformDisplay::rebuildWaveformPath()
{
    channelPaths.clear();
    const juce::ScopedLock sl(document.getLock());
    auto& buf = document.getBuffer();
    int numCh = buf.getNumChannels();
    int w = getWidth();
    int h = getHeight();
    lastPathWidth = w;
    lastPathHeight = h;
    if (numCh <= 0 || buf.getNumSamples() <= 0 || w <= 0 || h <= 0)
        return;

    int laneHeight = h / numCh;
    int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    double samplesPerPixel = (double) rangeLen / (double) w;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* data = buf.getReadPointer(ch);
        float laneY = (float) (ch * laneHeight);
        float laneMid = laneY + (float) laneHeight * 0.5f;
        float laneHalf = (float) laneHeight * 0.48f;

        std::vector<float> mins((size_t) w), maxs((size_t) w);
        for (int x = 0; x < w; ++x)
        {
            int64_t s0 = viewStart + (int64_t) (x * samplesPerPixel);
            int64_t s1 = viewStart + (int64_t) ((x + 1) * samplesPerPixel);
            s0 = juce::jlimit((int64_t) 0, document.getNumSamples(), s0);
            s1 = juce::jlimit((int64_t) 0, document.getNumSamples(), s1);
            if (s1 <= s0)
                s1 = juce::jmin(document.getNumSamples(), s0 + 1);

            float mn = 0.0f, mx = 0.0f;
            if (s1 > s0)
            {
                mn = 1.0f;
                mx = -1.0f;
                for (int64_t s = s0; s < s1; ++s)
                {
                    float v = data[s];
                    mn = juce::jmin(mn, v);
                    mx = juce::jmax(mx, v);
                }
            }
            mins[(size_t) x] = mn;
            maxs[(size_t) x] = mx;
        }

        juce::Path p;
        p.startNewSubPath(0.0f, laneMid - maxs[0] * laneHalf);
        for (int x = 1; x < w; ++x)
            p.lineTo((float) x, laneMid - maxs[(size_t) x] * laneHalf);
        for (int x = w - 1; x >= 0; --x)
            p.lineTo((float) x, laneMid - mins[(size_t) x] * laneHalf);
        p.closeSubPath();

        channelPaths.push_back(std::move(p));
    }
}

void WaveformDisplay::resized()
{
    rebuildWaveformPath();
}

void WaveformDisplay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1e23));

    if (document.isEmpty())
    {
        g.setColour(juce::Colours::grey);
        g.drawText("No audio loaded - press Record, or Open a file", getLocalBounds(), juce::Justification::centred);
        return;
    }

    int numCh = juce::jmax(1, document.getNumChannels());
    int laneHeight = getHeight() / numCh;

    g.setColour(juce::Colours::white.withAlpha(0.10f));
    for (auto m : document.chopMarkers)
    {
        if (m < viewStart || m > viewEnd)
            continue;
        float x = sampleToX(m);
        g.drawVerticalLine((int) x, 0.0f, (float) getHeight());
    }

    g.setColour(juce::Colour(0xff5ec2ff));
    for (auto& p : channelPaths)
        g.fillPath(p);

    g.setColour(juce::Colours::black.withAlpha(0.5f));
    for (int ch = 1; ch < numCh; ++ch)
        g.drawHorizontalLine(ch * laneHeight, 0.0f, (float) getWidth());

    // Selection: a wash, plus a bracket at each edge — a 2 px line with a small handle pill
    // at top and bottom (same as Sieve's editor).
    if (document.hasSelection())
    {
        const juce::Colour accent (0xff5ec2ff);
        const float h  = (float) getHeight();
        const float x0 = sampleToX(document.getSelectionStart());
        const float x1 = sampleToX(document.getSelectionEnd());

        g.setColour(accent.withAlpha(0.16f));
        g.fillRect(juce::Rectangle<float>(x0, 0.0f, juce::jmax(1.0f, x1 - x0), h));

        for (float x : { x0, x1 })
        {
            if (x < -2.0f || x > (float) getWidth() + 2.0f)
                continue;
            g.setColour(accent.withAlpha(0.9f));
            g.fillRect(juce::Rectangle<float>(x - 1.0f, 0.0f, 2.0f, h));

            constexpr float hw = 5.0f, hh = 14.0f;
            g.setColour(accent);
            for (float cy : { hh * 0.5f + 1.0f, h - hh * 0.5f - 1.0f })
                g.fillRoundedRectangle(x - hw * 0.5f, cy - hh * 0.5f, hw, hh, 2.0f);
        }
    }

    if (document.loopEnabled && document.loopEnd > document.loopStart)
    {
        g.setColour(juce::Colours::orange.withAlpha(0.9f));
        g.drawVerticalLine((int) sampleToX(document.loopStart), 0.0f, (float) getHeight());
        g.drawVerticalLine((int) sampleToX(document.loopEnd), 0.0f, (float) getHeight());
    }

    g.setColour(juce::Colours::red);
    g.drawVerticalLine((int) sampleToX(document.playhead.load()), 0.0f, (float) getHeight());
}

WaveformDisplay::EdgeHit WaveformDisplay::hitEdge(float pressX, float startX, float endX, float tolerance)
{
    const float dStart = juce::jmax(pressX - startX, startX - pressX);
    const float dEnd   = juce::jmax(pressX - endX,   endX - pressX);
    if (dStart > tolerance && dEnd > tolerance)
        return EdgeHit::newSelection;
    return dStart <= dEnd ? EdgeHit::resizeStart : EdgeHit::resizeEnd;
}

void WaveformDisplay::mouseDown(const juce::MouseEvent& e)
{
    const int64_t f = xToSample((float) e.x);

    if (document.hasSelection())
    {
        const float sx = sampleToX(document.getSelectionStart());
        const float ex = sampleToX(document.getSelectionEnd());
        switch (hitEdge((float) e.x, sx, ex, edgeTolerancePx))
        {
            case EdgeHit::resizeStart:
                dragKind = DragKind::resizeStart; dragAnchor = document.getSelectionEnd();   return;
            case EdgeHit::resizeEnd:
                dragKind = DragKind::resizeEnd;   dragAnchor = document.getSelectionStart(); return;
            case EdgeHit::newSelection:
                break;
        }
    }

    // The playhead move / selection clear waits for mouseUp, so we can tell a click from a drag.
    dragKind = DragKind::newSelection;
    dragAnchor = f;
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (dragKind == DragKind::none)
        return;

    const int64_t f  = xToSample((float) e.x);
    const int64_t lo = juce::jmin(dragAnchor, f);
    const int64_t hi = juce::jmax(dragAnchor, f);

    if (dragKind == DragKind::newSelection)
        document.setSelection(lo, hi);                    // end <= start reads as "no selection"
    else
        document.setSelection(lo, juce::jmax(hi, lo + 1));
}

void WaveformDisplay::mouseUp(const juce::MouseEvent& e)
{
    const DragKind kind = dragKind;
    dragKind = DragKind::none;
    if (kind == DragKind::none)
        return;

    const int64_t f = xToSample((float) e.x);

    if (kind == DragKind::newSelection)
    {
        const double framesPerPixel = (double) juce::jmax((int64_t) 1, viewEnd - viewStart)
                                    / (double) juce::jmax(1, getWidth());
        const int64_t slop = juce::jmax((int64_t) 1, (int64_t) (3.0 * framesPerPixel));
        int64_t moved = dragAnchor - f;
        if (moved < 0) moved = -moved;
        if (moved <= slop)                                // a click, not a drag
        {
            document.clearSelection();
            document.playhead = f;
            document.notifyChanged();
            return;
        }
    }

    if (document.hasSelection() && onSelectionCommitted)
        onSelectionCommitted();
}

void WaveformDisplay::mouseMove(const juce::MouseEvent& e)
{
    bool nearEdge = false;
    if (document.hasSelection())
    {
        const float sx = sampleToX(document.getSelectionStart());
        const float ex = sampleToX(document.getSelectionEnd());
        nearEdge = juce::jmax((float) e.x - sx, sx - (float) e.x) <= edgeTolerancePx
                || juce::jmax((float) e.x - ex, ex - (float) e.x) <= edgeTolerancePx;
    }
    setMouseCursor(nearEdge ? juce::MouseCursor::LeftRightResizeCursor
                            : juce::MouseCursor::NormalCursor);
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent&)
{
    document.setSelection(0, document.getNumSamples());
    if (onSelectionCommitted)
        onSelectionCommitted();
}

void WaveformDisplay::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (document.getNumSamples() <= 0)
        return;

    float dx = wheel.deltaX, dy = wheel.deltaY;
    if (wheel.isReversed) { dx = -dx; dy = -dy; }

    // Horizontal swipe / Shift-wheel → pan. Vertical wheel → zoom toward the pointer
    // (Sieve's editor: no modifier needed; Cmd/Ctrl still zooms too).
    if (juce::jmax(dx, -dx) > juce::jmax(dy, -dy))
    {
        panByPixels(dx * (float) juce::jmax(1, getWidth()) * 0.5f);
        return;
    }

    const double factor = juce::jlimit(0.5, 2.0, std::exp(-dy * 1.4));   // dy>0 (wheel up) => zoom in
    zoomToward(factor, (float) e.x);
}

void WaveformDisplay::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (viewEnd <= viewStart || viewEnd > document.getNumSamples())
    {
        viewStart = 0;
        viewEnd = document.getNumSamples();
    }
    rebuildWaveformPath();
    repaint();
}
