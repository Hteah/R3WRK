#include "WaveformDisplay.h"
#include "EditActions.h"
#include <cmath>

WaveformDisplay::WaveformDisplay(AudioDocument& doc) : document(doc)
{
    document.changeBroadcaster.addChangeListener(this);
    theme->addChangeListener(this);
    viewEnd = lastKnownTotal = document.getNumSamples();
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

WaveformDisplay::~WaveformDisplay()
{
    theme->removeChangeListener(this);
    document.changeBroadcaster.removeChangeListener(this);
}

// If the buffer version just changed (a real edit, not a selection/playhead move) and the
// view was showing (at or near) the whole document just beforehand, keep showing the whole
// document. Without this, an edit that changes the document's length -- an extreme Stretch,
// say -- silently leaves the view pinned to the *old* length: the new, longer document is
// still there, just off the right edge, with no way back to the full picture short of a lot
// of manual zooming out. A view the user had deliberately zoomed into a sub-region is left
// alone (this only fires when the old view covered everything). Returns whether the content
// actually changed, so callers know whether to rebuild the waveform path.
bool WaveformDisplay::refitViewIfContentChanged()
{
    const int version = document.getBufferVersion();
    if (version == lastBufferVersion)
        return false;

    const bool wasFullView = viewStart <= 0
                          && viewEnd >= effectiveSpanFor(lastKnownTotal, document.getTimeScale());
    lastBufferVersion = version;
    lastKnownTotal = document.getNumSamples();

    if (wasFullView)
    {
        viewStart = 0;
        viewEnd = juce::jmax((int64_t) 1, maxViewSpan());
    }
    return true;
}

void WaveformDisplay::timerCallback()
{
    // Rebuild whenever the audio content changed, or the view range is stale / degenerate,
    // or the component was resized before the buffer existed. Cheaper than trusting only the
    // async ChangeBroadcaster, and self-heals any ordering race on first load.
    const bool contentChanged = refitViewIfContentChanged();

    // Same "was showing everything -> keep showing everything" idea as
    // refitViewIfContentChanged(), but for the KnobRow's Speed/Pitch/Stretch knobs, which
    // change getTimeScale() (and so maxViewSpan()) without ever bumping bufferVersion -- a
    // live playback/visual effect, not an edit. This 30Hz poll is the only place that notices
    // a knob move at all (KnobRow writes the atomics directly with no change broadcast), so
    // the refit has to happen here rather than in refitViewIfContentChanged().
    const double timeScale = document.getTimeScale();
    const bool timeScaleChanged = std::abs(timeScale - lastTimeScale) > 1.0e-9;
    if (timeScaleChanged)
    {
        const bool wasFullView = viewStart <= 0
                              && viewEnd >= effectiveSpanFor(document.getNumSamples(), lastTimeScale);
        lastTimeScale = timeScale;
        if (wasFullView)
        {
            viewStart = 0;
            viewEnd = juce::jmax((int64_t) 1, maxViewSpan());
        }
    }

    const int64_t maxSpan = maxViewSpan();
    const bool viewBad = viewEnd <= viewStart || viewEnd > maxSpan || viewStart >= juce::jmax((int64_t) 1, maxSpan);

    // A freshly (or no longer) available real-stretch preview also means the drawn shape is
    // stale -- see rebuildWaveformPath() for how it's used.
    const bool previewChanged = stretchPreview.update();

    if (contentChanged || timeScaleChanged || viewBad || previewChanged
        || getWidth() != lastPathWidth || getHeight() != lastPathHeight)
    {
        if (viewBad)   // refits above already handled the "was showing everything" cases
        {
            viewStart = 0;
            viewEnd = juce::jmax((int64_t) 1, maxSpan);
        }
        rebuildWaveformPath();
    }
    repaint();
}

// Sample <-> pixel mapping. `viewStart/viewEnd` are always the raw (stored-audio) sample
// bounds of the current zoom -- unaffected by the playback knobs, same as always. What
// changes is how many of those raw samples actually fit across the component's pixel
// width: at getTimeScale() > 1 (stretched longer), fewer of them do, so the same raw span
// is spread across more horizontal space than the viewport shows -- visually "zoomed in",
// exactly like a slowed-down sample looking longer on a hardware sampler. At < 1 (sped up)
// more of them fit, leaving blank space -- the sample looks shorter. (That's the raw
// mechanism for *any* view span; see effectiveSpanFor() below for the specific span that
// makes the "zoomed all the way out" view show the whole raw buffer with neither blank
// space nor unreachable content, whatever timeScale currently is.)
int64_t WaveformDisplay::xToSample(float x) const
{
    const int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    const double samplesPerPixel = (double) rangeLen / (double) juce::jmax(1, getWidth())
                                  / juce::jmax(0.0001, document.getTimeScale());
    return juce::jlimit((int64_t) 0, document.getNumSamples(),
                        viewStart + (int64_t) ((double) x * samplesPerPixel));
}

float WaveformDisplay::sampleToX(int64_t sample) const
{
    const int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    const double samplesPerPixel = (double) rangeLen / (double) juce::jmax(1, getWidth())
                                  / juce::jmax(0.0001, document.getTimeScale());
    return (float) ((double) (sample - viewStart) / samplesPerPixel);
}

// The view span ("zoomed all the way out") that makes the raw audio exactly fill the
// component's width, however Speed/Pitch/Stretch currently have it visually reshaped --
// rawTotal*timeScale. rebuildWaveformPath()/xToSample() divide samples-per-pixel by
// timeScale, so with rangeLen = rawTotal*timeScale, samplesPerPixel works out to exactly
// rawTotal/width regardless of timeScale -- the last pixel's cursor lands exactly on
// rawTotal, so the *entire* raw buffer renders edge to edge, neither with unreachable
// content left over past the last pixel (timeScale > 1: a plain rawTotal span would only
// render the first rawTotal/timeScale of it, the rest silently never drawn no matter how far
// out you "zoom") nor with dead blank space left over before it (timeScale < 1: a plain
// rawTotal span finishes rendering all of it partway across, wasting the rest of the width).
// The zoom/pan clamps below all treat this as the hard "zoomed all the way out" limit in
// both directions, same as they'd treat rawTotal itself at timeScale == 1.
int64_t WaveformDisplay::effectiveSpanFor(int64_t rawTotal, double timeScale) const
{
    timeScale = juce::jmax(0.0001, timeScale);
    return juce::jmax((int64_t) 1, (int64_t) ((double) rawTotal * timeScale));
}

int64_t WaveformDisplay::maxViewSpan() const
{
    return effectiveSpanFor(document.getNumSamples(), document.getTimeScale());
}

void WaveformDisplay::zoomToFit()
{
    viewStart = 0;
    viewEnd = juce::jmax((int64_t) 1, maxViewSpan());
    rebuildWaveformPath();
    repaint();
}

// Keyboard zoom (⌘+/⌘-, see PluginEditor::keyPressed) -- routes through the exact same
// zoomToward() the mouse wheel uses, so a keypress behaves like one wheel notch: hovering
// near a selection edge pins and zooms into that edge specifically, left or right. Where it
// differs from the wheel: there's no real pointer to speak of (you're using the keyboard
// specifically to keep your hand off the mouse), so the "pointer" fed to zoomToward() is the
// selection's own midpoint whenever there is one, not wherever the mouse incidentally sits.
// zoomToward()'s existing framing step already centres on that midpoint the *first* time
// (while the view is wider than the selection), but once you're zoomed in tighter than it,
// its normal pointer-anchored zoom takes over -- with the real mouse position that'd usually
// be fine (you're pointing at something meaningful), but for a keyboard press it would just
// pin whatever arbitrary spot the mouse happens to be, drifting the view away from the
// selection with every further press. Anchoring on the midpoint throughout keeps every
// keyboard zoom level centred on the selection, matching Sieve's editor. No selection ->
// falls back to the actual mouse position, same as before. The factor is gentler than a
// wheel notch can be (that one scales with how hard/fast the user scrolls, up to +/-2x) --
// fixed, modest steps here so repeated presses zoom in comfortably small increments. Kept as
// an exact inverse pair (0.9 and 1/0.9) so zooming in then out lands back on the same span.
static constexpr double keyboardZoomStep = 0.9;
void WaveformDisplay::zoomIn()  { zoomToward(keyboardZoomStep,       keyboardZoomAnchorX()); }
void WaveformDisplay::zoomOut() { zoomToward(1.0 / keyboardZoomStep, keyboardZoomAnchorX()); }

float WaveformDisplay::currentMouseX() const
{
    return juce::jlimit(0.0f, (float) juce::jmax(1, getWidth()), (float) getMouseXYRelative().x);
}

float WaveformDisplay::keyboardZoomAnchorX() const
{
    if (document.hasSelection())
        return sampleToX((document.getSelectionStart() + document.getSelectionEnd()) / 2);
    return currentMouseX();
}

// Sieve-style editor zoom: `spanFactor` multiplies the visible span (<1 = zoom in).
//   - zooming in, pointer near a selection bracket -> pin that bracket and zoom into it (no
//     span limit), so the edge you're pointing at becomes the focus;
//   - zooming in elsewhere, selection still narrower than the view -> frame the whole
//     selection so the wheel pulls you into it;
//   - zooming out with a selection -> reveal whichever side of it the pointer is on, so you
//     can deliberately walk back out to either side just by choosing where to point;
//   - otherwise -> keep the sample under the pointer fixed.
void WaveformDisplay::zoomToward(double spanFactor, float pointerX)
{
    const int64_t total = document.getNumSamples();
    if (total <= 0)
        return;

    spanFactor = juce::jlimit(0.5, 2.0, spanFactor);
    const int64_t curLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    const int64_t newLen = (int64_t) juce::jlimit(16.0, (double) maxViewSpan(), (double) curLen * spanFactor);
    const double timeScale = juce::jmax(0.0001, document.getTimeScale());

    // anchorFrac is a fraction of the pixel width; convert it to a raw-sample offset via
    // the same timeScale-aware density xToSample()/sampleToX() use, so the anchor sample
    // stays pinned under the pointer even when the waveform is visually stretched.
    auto applyView = [&](int64_t anchorSample, double anchorFrac)
    {
        int64_t newStart = anchorSample - (int64_t) (anchorFrac * (double) newLen / timeScale);
        newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, maxViewSpan() - newLen), newStart);
        viewStart = newStart;
        viewEnd = newStart + newLen;
        rebuildWaveformPath();
        repaint();
    };

    if (document.hasSelection())
    {
        const int64_t selStart = document.getSelectionStart();
        const int64_t selEnd = document.getSelectionEnd();

        // Zooming in near a bracket: lock onto that edge and zoom into it, keeping it
        // comfortably in view (nudged off the very edges of the component). No
        // selection-span clamp here, so the wheel can take you right down onto the sample.
        // Gated to zoom-*in* only -- this used to also fire on zoom-out, which is where
        // "zooming out always drifts back to the left edge no matter where the pointer is"
        // came from: bracket-grab pins whichever edge is within bracketGrabPx of the
        // pointer at a fraction *derived from that same on-screen position*, clamped to at
        // least 0.15 -- so once a zoom-in had you hugging the left bracket, continuing to
        // scroll (zooming out now, mouse not having moved) kept re-triggering the same
        // "near the left bracket" case and re-pinning it near the left of the screen again,
        // regardless of intent. Zoom-out has its own, deliberate handling below instead.
        if (spanFactor < 1.0)
        {
            const float sx = sampleToX(selStart);
            const float ex = sampleToX(selEnd);
            const float dStart = std::abs(pointerX - sx);
            const float dEnd   = std::abs(pointerX - ex);
            constexpr float bracketGrabPx = 12.0f;

            if (juce::jmin(dStart, dEnd) <= bracketGrabPx)
            {
                const int64_t edge = (dStart <= dEnd) ? selStart : selEnd;
                const double frac = juce::jlimit(0.15, 0.85,
                                                 (double) pointerX / (double) juce::jmax(1, getWidth()));
                applyView(edge, frac);
                return;
            }
            // Not near an edge: no special selection handling here any more -- falls
            // through to the plain pointer-anchored zoom at the bottom of the function,
            // same as zooming anywhere else. There used to be a "frame the whole selection"
            // step here (jump straight to a view sized to fit it, the moment the current
            // view was wider than the selection), but that condition was true for nearly
            // the *entire* zoomed-out range for a small selection in a longer file, not just
            // the last step before naturally reaching that size -- so the very first zoom-in
            // tick from anywhere zoomed out would jump straight to "just the selection"
            // instead of zooming in gradually. The plain pointer-anchored zoom already tracks
            // the pointer correctly whether it's inside the selection or not (see xToSample()
            // below), which is all "zoom toward the selection" ever really needed.
        }
        else if (selEnd > selStart)
        {
            // Zooming out: reveal whichever side of the selection the pointer is on, rather
            // than leaving it to chance which side ends up in view -- pin the near selection
            // edge at a fixed, comfortable fraction of the width (not derived from the
            // pointer's exact position, so it can't degenerate into hugging one edge the way
            // the old shared bracket-grab logic did) so repeated zoom-out steps keep opening
            // up that same side: pointer on the left half opens up what's before the
            // selection, right half opens up what's after.
            const bool pointerOnLeft = pointerX < (float) juce::jmax(1, getWidth()) * 0.5f;
            applyView(pointerOnLeft ? selStart : selEnd, pointerOnLeft ? 0.7 : 0.3);
            return;
        }
    }

    const double frac = juce::jlimit(0.0, 1.0, (double) pointerX / (double) juce::jmax(1, getWidth()));
    applyView(xToSample(pointerX), frac);
}

void WaveformDisplay::panByPixels(float dxPixels)
{
    const int64_t len = viewEnd - viewStart;
    if (len <= 0 || len >= maxViewSpan())
        return;

    const double framesPerPixel = (double) len / (double) juce::jmax(1, getWidth())
                                 / juce::jmax(0.0001, document.getTimeScale());
    int64_t newStart = viewStart - (int64_t) (dxPixels * framesPerPixel);
    newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, maxViewSpan() - len), newStart);
    viewStart = newStart;
    viewEnd = newStart + len;
    rebuildWaveformPath();
    repaint();
}

void WaveformDisplay::rebuildPeakCache()
{
    juce::AudioBuffer<float> copy;
    {
        const juce::ScopedLock sl(document.getLock());   // held only for the copy, not the scan
        copy.makeCopyOf(document.getBuffer());
        peakVersion = document.getBufferVersion();
    }

    const int numCh = juce::jmax(0, copy.getNumChannels());
    const int64_t total = copy.getNumSamples();
    peakTotalSamples = total;

    const int nBins = (int) ((total + peakBinSize - 1) / peakBinSize);
    chPeakMin.assign((size_t) numCh, std::vector<float>((size_t) juce::jmax(0, nBins), 0.0f));
    chPeakMax.assign((size_t) numCh, std::vector<float>((size_t) juce::jmax(0, nBins), 0.0f));

    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = copy.getReadPointer(ch);
        auto& mn = chPeakMin[(size_t) ch];
        auto& mx = chPeakMax[(size_t) ch];
        for (int b = 0; b < nBins; ++b)
        {
            const int64_t s0 = (int64_t) b * peakBinSize;
            const int64_t s1 = juce::jmin(total, s0 + peakBinSize);
            const auto r = juce::FloatVectorOperations::findMinAndMax(d + s0, (int) (s1 - s0));
            mn[(size_t) b] = r.getStart();
            mx[(size_t) b] = r.getEnd();
        }
    }
}

void WaveformDisplay::rebuildWaveformPath()
{
    channelPaths.clear();

    const int w = getWidth();
    const int h = getHeight();
    lastPathWidth = w;
    lastPathHeight = h;

    if (peakVersion != document.getBufferVersion())
        rebuildPeakCache();

    const int numCh = (int) chPeakMin.size();
    const int64_t total = peakTotalSamples;
    if (numCh <= 0 || total <= 0 || w <= 0 || h <= 0)
        return;

    const int laneHeight = h / numCh;
    const int64_t rangeLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    // Raw samples per pixel, folding in the visual time-stretch: at getTimeScale() > 1 the
    // waveform is drawn "spread out" (fewer raw samples per pixel), at < 1 "squeezed in".
    const double timeScale = juce::jmax(0.0001, document.getTimeScale());
    const double samplesPerPixel = (double) rangeLen / (double) w / timeScale;

    // While Speed/Pitch/Stretch are non-identity and settled, draw the *real* processed
    // shape instead of the stored audio rescaled -- see WaveformStretchPreview. Its peak
    // cache covers the actual RubberBand output for the whole document at the current knob
    // position, so a raw sample position s maps to processed-domain index s*timeScale (the
    // same factor rescales the view span itself, see effectiveSpanFor()/maxViewSpan() above).
    const bool usePreview = stretchPreview.hasPreview();

    // Deep zoom (fewer than one peak bin per pixel): copy just the actually-visible span
    // (w * samplesPerPixel raw samples -- bounded even at extreme Stretch, unlike the full
    // [viewStart,viewEnd) range) out from under the lock once, then read it per-sample below.
    // Not needed at all when drawing from the preview buffer instead.
    juce::AudioBuffer<float> raw;
    int64_t rawStart = 0;
    if (! usePreview && samplesPerPixel < (double) peakBinSize)
    {
        rawStart = juce::jlimit((int64_t) 0, total, viewStart);
        const int64_t visibleSamples = (int64_t) (samplesPerPixel * (double) w) + 2;
        const int rawLen = (int) juce::jlimit((int64_t) 0, total - rawStart, visibleSamples);
        const juce::ScopedLock sl(document.getLock());
        auto& src = document.getBuffer();
        const int copyLen = juce::jmin(rawLen, src.getNumSamples() - (int) rawStart);
        if (copyLen > 0)
        {
            raw.setSize(src.getNumChannels(), copyLen);
            for (int ch = 0; ch < src.getNumChannels(); ++ch)
                raw.copyFrom(ch, 0, src, ch, (int) rawStart, copyLen);
        }
    }

    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto& binMin = chPeakMin[(size_t) ch];
        const auto& binMax = chPeakMax[(size_t) ch];
        const int nBins = (int) binMin.size();

        const float laneMid  = (float) (ch * laneHeight) + (float) laneHeight * 0.5f;
        const float laneHalf = (float) laneHeight * 0.48f;

        std::vector<float> mins((size_t) w, 0.0f), maxs((size_t) w, 0.0f);
        for (int x = 0; x < w; ++x)
        {
            int64_t s0 = viewStart + (int64_t) (x       * samplesPerPixel);
            int64_t s1 = viewStart + (int64_t) ((x + 1) * samplesPerPixel);
            s0 = juce::jlimit((int64_t) 0, total, s0);
            s1 = juce::jlimit((int64_t) 0, total, s1);
            if (s1 <= s0)
                s1 = juce::jmin(total, s0 + 1);

            float mn = 0.0f, mx = 0.0f;
            if (usePreview && ch < stretchPreview.getNumChannels())
            {
                const int64_t p0 = (int64_t) ((double) s0 * timeScale);
                const int64_t p1 = (int64_t) ((double) s1 * timeScale);
                stretchPreview.getPeakRange(ch, p0, p1, mn, mx);
            }
            else
            {
                const int64_t r0 = s0 - rawStart, r1 = s1 - rawStart;
                if (raw.getNumSamples() > 0 && ch < raw.getNumChannels()
                    && r0 >= 0 && r1 <= raw.getNumSamples() && r1 > r0)
                {
                    const auto r = juce::FloatVectorOperations::findMinAndMax(raw.getReadPointer(ch) + r0,
                                                                             (int) (r1 - r0));
                    mn = r.getStart();
                    mx = r.getEnd();
                }
                else if (nBins > 0)
                {
                    int b0 = juce::jlimit(0, nBins - 1, (int) (s0 / peakBinSize));
                    int b1 = juce::jlimit(b0, nBins - 1, (int) ((s1 - 1) / peakBinSize));
                    for (int b = b0; b <= b1; ++b)
                    {
                        mn = juce::jmin(mn, binMin[(size_t) b]);
                        mx = juce::jmax(mx, binMax[(size_t) b]);
                    }
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

void WaveformDisplay::paintRecordingScope(juce::Graphics& g)
{
    const auto& pal = theme->palette();

    juce::Path clip;
    clip.addRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
    g.reduceClipRegion(clip);

    g.fillAll(pal.panelBg);

    const float W = (float) getWidth();
    const float H = (float) getHeight();
    const float mid = std::floor(H * 0.5f) + 0.5f;
    const float half = H * 0.46f;

    g.setColour(pal.zeroLine);
    g.drawHorizontalLine((int) mid, 0.0f, W);

    const int n = AudioDocument::scopeSize;
    const int wpos = document.scopeWritePos.load(std::memory_order_acquire);

    juce::Path p;
    p.startNewSubPath(0.0f, mid);
    for (int i = 0; i < n; ++i)
    {
        const int idx = (wpos + i) % n;
        const float x = W * (float) i / (float) (n - 1);
        p.lineTo(x, mid - juce::jlimit(-1.0f, 1.0f, document.scopeMax[idx]) * half);
    }
    for (int i = n - 1; i >= 0; --i)
    {
        const int idx = (wpos + i) % n;
        const float x = W * (float) i / (float) (n - 1);
        p.lineTo(x, mid - juce::jlimit(-1.0f, 1.0f, document.scopeMin[idx]) * half);
    }
    p.closeSubPath();
    g.setColour(pal.waveform);
    g.fillPath(p);

    const double sr  = document.getSampleRate() > 0.0 ? document.getSampleRate() : 44100.0;
    const double sec = (double) document.recordedSamples.load(std::memory_order_relaxed) / sr;
    const int    m   = (int) (sec / 60.0);
    const juce::String clock = juce::String::formatted("%d:%05.2f", m, sec - m * 60.0);

    g.setColour(pal.playhead);
    g.fillEllipse(12.0f, 12.0f, 9.0f, 9.0f);
    g.setColour(pal.screenText);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText("REC  " + clock, 28, 8, 220, 18, juce::Justification::centredLeft, false);
}

// Live preview of a pending Amplify/Stretch-Pitch edit, drawn over the selection while its
// pop-up panel's slider is being dragged (see AudioDocument::previewActive and
// EditorToolbar's AmplifyPanel/StretchPanel). Rebuilt from a fresh raw copy of just the
// selection's samples every repaint, rather than the whole-buffer peak cache the committed
// waveform draws from -- selections are typically far smaller than the whole clip, so this
// stays cheap, and gives full-resolution preview regardless of how deep the view is zoomed.
//
// Amplify scales the peaks in place (same pixel span as the real selection). Stretch instead
// redraws that same audio spread across a wider or narrower span starting at the selection's
// left edge -- a direct preview of "this audio, once stretched, would occupy this much room
// and roughly look like this" -- with a dashed marker at the new right edge so it reads as a
// preview, not the committed selection bound.
void WaveformDisplay::paintSelectionPreview(juce::Graphics& g)
{
    if (! document.previewActive || ! document.hasSelection())
        return;

    const auto sel = document.getSelection();
    const int64_t selLen = sel.getEnd() - sel.getStart();
    if (selLen <= 0)
        return;

    juce::AudioBuffer<float> raw;
    {
        const juce::ScopedLock sl(document.getLock());
        auto& src = document.getBuffer();
        const int len = (int) juce::jlimit((int64_t) 0, (int64_t) src.getNumSamples() - sel.getStart(), selLen);
        if (len <= 0)
            return;
        raw.setSize(src.getNumChannels(), len);
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
            raw.copyFrom(ch, 0, src, ch, (int) sel.getStart(), len);
    }
    if (raw.getNumSamples() <= 0)
        return;

    const float x0 = sampleToX(sel.getStart());
    const float normalWidth = juce::jmax(1.0f, sampleToX(sel.getEnd()) - x0);
    const float previewWidth = juce::jmax(1.0f, normalWidth * (float) document.previewStretchRatio);
    const int pxStart = (int) juce::jlimit(0.0f, (float) getWidth(), x0);
    const int pxEnd   = (int) juce::jlimit(0.0f, (float) getWidth(), x0 + previewWidth);
    const int n = pxEnd - pxStart;
    if (n <= 0)
        return;

    const double samplesPerPixel = (double) raw.getNumSamples() / (double) previewWidth;
    const int numCh = juce::jmax(1, document.getNumChannels());
    const int laneHeight = getHeight() / numCh;
    const float gain = document.previewGainLinear;

    std::vector<float> mins((size_t) n), maxs((size_t) n);
    for (int ch = 0; ch < juce::jmin(numCh, raw.getNumChannels()); ++ch)
    {
        const float* d = raw.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
        {
            const double relX = (double) (pxStart + i) - (double) x0;
            int64_t s0 = (int64_t) (relX * samplesPerPixel);
            int64_t s1 = (int64_t) ((relX + 1.0) * samplesPerPixel);
            s0 = juce::jlimit((int64_t) 0, (int64_t) raw.getNumSamples(), s0);
            s1 = juce::jlimit(s0, (int64_t) raw.getNumSamples(), s1);

            float mn = 0.0f, mx = 0.0f;
            if (s1 > s0)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(d + s0, (int) (s1 - s0));
                mn = r.getStart();
                mx = r.getEnd();
            }
            mins[(size_t) i] = juce::jlimit(-2.0f, 2.0f, mn * gain);
            maxs[(size_t) i] = juce::jlimit(-2.0f, 2.0f, mx * gain);
        }

        const float laneMid  = (float) (ch * laneHeight) + (float) laneHeight * 0.5f;
        const float laneHalf = (float) laneHeight * 0.48f;

        juce::Path p;
        p.startNewSubPath((float) pxStart, laneMid - maxs[0] * laneHalf);
        for (int i = 1; i < n; ++i)
            p.lineTo((float) (pxStart + i), laneMid - maxs[(size_t) i] * laneHalf);
        for (int i = n - 1; i >= 0; --i)
            p.lineTo((float) (pxStart + i), laneMid - mins[(size_t) i] * laneHalf);
        p.closeSubPath();

        g.setColour(theme->palette().accent.withAlpha(0.85f));
        g.fillPath(p);
    }

    if (std::abs(document.previewStretchRatio - 1.0) > 1.0e-6)
    {
        const float dashes[] = { 3.0f, 3.0f };
        g.setColour(theme->palette().accent);
        g.drawDashedLine(juce::Line<float>((float) pxEnd, 0.0f, (float) pxEnd, (float) getHeight()),
                         dashes, 2, 1.5f);
    }
}

void WaveformDisplay::paint(juce::Graphics& g)
{
    if (document.isRecording.load(std::memory_order_relaxed))
    {
        paintRecordingScope(g);
        return;
    }

    const auto& pal = theme->palette();

    juce::Path clip;
    clip.addRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
    g.reduceClipRegion(clip);

    g.fillAll(pal.panelBg);

    if (document.isEmpty())
    {
        g.setColour(pal.screenTextDim);
        g.drawText("No audio loaded - press Record, or Open a file", getLocalBounds(), juce::Justification::centred);
        return;
    }

    int numCh = juce::jmax(1, document.getNumChannels());
    int laneHeight = getHeight() / numCh;

    // Zero-amplitude reference line through each channel lane (drawn behind the wave, so it
    // shows through the quiet parts and the wave sits on it).
    {
        const float dashes[] = { 2.0f, 3.0f };
        g.setColour(pal.zeroLine);
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float y = std::floor((float) ch * (float) laneHeight + (float) laneHeight * 0.5f) + 0.5f;
            g.drawDashedLine(juce::Line<float>(0.0f, y, (float) getWidth(), y), dashes, 2, 1.0f);
        }
    }

    g.setColour(pal.waveform);
    for (auto& p : channelPaths)
        g.fillPath(p);

    g.setColour(pal.gridLine);
    for (int ch = 1; ch < numCh; ++ch)
        g.drawHorizontalLine(ch * laneHeight, 0.0f, (float) getWidth());

    paintSelectionPreview(g);

    // Selection: a wash, plus a bracket at each edge — a 2 px line with a small handle pill
    // at top and bottom (same as Sieve's editor).
    if (document.hasSelection())
    {
        const juce::Colour accent = pal.accent;
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
        g.setColour(pal.loopMarker.withAlpha(0.9f));
        g.drawVerticalLine((int) sampleToX(document.loopStart), 0.0f, (float) getHeight());
        g.drawVerticalLine((int) sampleToX(document.loopEnd), 0.0f, (float) getHeight());
    }

    // Slice markers (right-click to place; Tools -> Slice to Folder / Export Octatrack Chain):
    // a thin accent line with a small downward flag at the top edge so they read as markers,
    // not selection brackets.
    {
        const auto& marks = document.getSliceMarkers();
        const float h = (float) getHeight();
        for (int64_t m : marks)
        {
            const float x = sampleToX(m);
            if (x < -6.0f || x > (float) getWidth() + 6.0f)
                continue;
            g.setColour(pal.accent);
            g.fillRect(juce::Rectangle<float>(x - 0.5f, 0.0f, 1.0f, h));
            juce::Path flag;
            flag.addTriangle(x - 4.0f, 0.0f, x + 4.0f, 0.0f, x, 7.0f);
            g.fillPath(flag);
        }
    }

    g.setColour(pal.playhead);
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
    if (document.scrubModeEnabled)
    {
        document.isPlaying = false;   // scrubbing and normal playback don't mix
        scrubAnchorX = (float) e.x;   // the shuttle's "centre" -- see mouseDrag()
        document.playhead = xToSample((float) e.x);
        document.scrubVelocity = 0.0;   // dead centre -- silent until you pull away from it
        document.isScrubbing = true;
        document.notifyChanged();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (document.hasSelection())
        {
            const float sx = sampleToX(document.getSelectionStart());
            const float ex = sampleToX(document.getSelectionEnd());
            if ((float) e.x > sx + edgeTolerancePx && (float) e.x < ex - edgeTolerancePx)
            {
                dragKind = DragKind::none;
                if (onSelectionContextMenu)
                    onSelectionContextMenu(e.getScreenPosition());
                return;
            }
        }

        showSliceMarkerMenu(e);
        return;
    }

    const int64_t f = xToSample((float) e.x);
    dragOutStarted = false;

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

        // Press inside the selection body -> a drag from here drags the audio out as a file.
        if ((float) e.x > sx + edgeTolerancePx && (float) e.x < ex - edgeTolerancePx)
        {
            dragKind = DragKind::dragOut;
            dragAnchor = f;
            return;
        }
    }

    // The playhead move / selection clear waits for mouseUp, so we can tell a click from a drag.
    dragKind = DragKind::newSelection;
    dragAnchor = f;
}

void WaveformDisplay::showSliceMarkerMenu(const juce::MouseEvent& e)
{
    const int64_t sample = xToSample((float) e.x);
    const int64_t tol    = juce::jmax((int64_t) 1,
                                      (xToSample((float) e.x + 6.0f) - xToSample((float) e.x - 6.0f)) / 2);
    const int nearIdx     = document.findSliceMarkerNear(sample, tol);
    const bool hasMarkers = ! document.getSliceMarkers().empty();

    enum { idAdd = 1, idDelete, idClear };
    juce::PopupMenu m;
    if (nearIdx >= 0)
        m.addItem(idDelete, "Delete slice marker");
    else
        m.addItem(idAdd, "Add slice marker");
    if (hasMarkers)
    {
        m.addSeparator();
        m.addItem(idClear, "Clear all slice markers");
    }

    const auto target = juce::Rectangle<int>(e.getScreenPosition().x, e.getScreenPosition().y, 1, 1);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(target),
                    [this, sample, nearIdx](int r)
                    {
                        switch (r)
                        {
                            case idAdd:    document.addSliceMarker(sample);      break;
                            case idDelete: document.removeSliceMarker(nearIdx);  break;
                            case idClear:  document.clearSliceMarkers();         break;
                            default: break;
                        }
                    });
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (document.scrubModeEnabled)
    {
        if (! document.isScrubbing.load())
            return;

        // Shuttle-style, not speed-of-motion: how *far* you've pulled away from where you
        // pressed sets the rate (sign = direction), same as a tape deck's shuttle wheel --
        // not literally "how fast the tape is moving under your finger". So a fast flick to a
        // modest distance starts just as slow as a careful pull to that same distance, and
        // holding the pointer still at some distance keeps playing at that rate rather than
        // decaying to zero -- only *how far*, never *how fast*, matters. (An earlier version
        // used raw pixel-delta / time-between-events, which blew past the velocity ceiling on
        // almost any drag once samples-per-pixel was more than tiny -- a several-pixel move
        // over one ~10ms mouse event is a huge sample delta at any real zoom level.)
        const float offsetPx = (float) e.x - scrubAnchorX;
        const float magnitude = juce::jlimit(0.0f, 1.0f,
            (std::abs(offsetPx) - scrubDeadZonePx) / (scrubMaxDragPx - scrubDeadZonePx));
        const float curved = magnitude * magnitude;   // slow near the centre, faster further out

        const double sampleRate = juce::jmax(1.0, document.getSampleRate());
        const double maxVelocity = sampleRate * 12.0;   // generous but finite -- see renderScrub()
        const double velocity = (offsetPx < 0.0f ? -1.0 : 1.0) * (double) curved * maxVelocity;

        document.scrubVelocity = velocity;
        return;
    }

    if (dragKind == DragKind::none)
        return;

    if (dragKind == DragKind::dragOut)
    {
        if (! dragOutStarted && e.getDistanceFromDragStart() > 6)
        {
            dragOutStarted = true;
            beginSelectionDragExport();
        }
        return;
    }

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
    if (document.scrubModeEnabled)
    {
        document.isScrubbing = false;
        document.scrubVelocity = 0.0;
        document.notifyChanged();   // repaint the playhead at wherever the drag ended
        return;
    }

    const DragKind kind = dragKind;
    dragKind = DragKind::none;
    if (kind == DragKind::none)
        return;

    const int64_t f = xToSample((float) e.x);

    if (kind == DragKind::dragOut)
    {
        if (! dragOutStarted)     // a click inside the selection, not a drag -> deselect + seek
        {
            document.clearSelection();
            document.playhead = f;
            document.notifyChanged();
        }
        return;
    }

    if (kind == DragKind::newSelection)
    {
        // Same timeScale-aware density xToSample() uses (this file's other frame/pixel
        // conversions all divide by it -- this one didn't, a real bug: at a high Stretch,
        // the raw-frame delta for a real few-pixel drag is *much* smaller than this slop
        // tolerance was computing without the division, so a genuine small selection at a
        // zoomed-out view kept reading as "just a click" and getting cleared instead of kept).
        const double framesPerPixel = (double) juce::jmax((int64_t) 1, viewEnd - viewStart)
                                    / (double) juce::jmax(1, getWidth())
                                    / juce::jmax(0.0001, document.getTimeScale());
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
    if (document.scrubModeEnabled)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);   // "drag left/right to scrub"
        return;
    }

    auto cursor = juce::MouseCursor::NormalCursor;
    if (document.hasSelection())
    {
        const float sx = sampleToX(document.getSelectionStart());
        const float ex = sampleToX(document.getSelectionEnd());
        const bool nearEdge = juce::jmax((float) e.x - sx, sx - (float) e.x) <= edgeTolerancePx
                           || juce::jmax((float) e.x - ex, ex - (float) e.x) <= edgeTolerancePx;
        if (nearEdge)
            cursor = juce::MouseCursor::LeftRightResizeCursor;
        else if ((float) e.x > sx && (float) e.x < ex)
            cursor = juce::MouseCursor::DraggingHandCursor;   // drag the selection out
    }
    setMouseCursor(cursor);
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent&)
{
    if (document.scrubModeEnabled)
        return;   // scrubbing repurposes every mouse gesture here; no select-all mid-scrub

    document.setSelection(0, document.getNumSamples());
    if (onSelectionCommitted)
        onSelectionCommitted();
}

void WaveformDisplay::beginSelectionDragExport()
{
    if (! document.hasSelection())
        return;

    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("R3WRK");
    dir.createDirectory();

    // Tidy: drop drag temp files older than 10 minutes.
    const auto nowMs = juce::Time::getCurrentTime().toMilliseconds();
    for (auto& old : dir.findChildFiles(juce::File::findFiles, false, "*.wav"))
        if (nowMs - old.getLastModificationTime().toMilliseconds() > 10 * 60 * 1000)
            old.deleteFile();

    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H.%M.%S");
    const auto file = dir.getChildFile("R3WRK selection " + stamp + ".wav").getNonexistentSibling();

    if (! EditActions::exportSelection(document, file))
        return;

    // canMoveFiles = false: the receiver copies it, so our temp file stays valid.
    juce::DragAndDropContainer::performExternalDragDropOfFiles({ file.getFullPathName() },
                                                              false, this, nullptr);
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

    // Lock the zoom anchor to wherever this gesture started, not the live pointer x on every
    // notch -- see the member comment on wheelGestureAnchorX for why.
    const uint32_t now = juce::Time::getMillisecondCounter();
    if (now - lastWheelEventMs > wheelGestureGapMs)
        wheelGestureAnchorX = (float) e.x;
    lastWheelEventMs = now;

    // dy>0 (wheel up) => zoom in. Was jlimit(0.5, 2.0, exp(-dy*1.4)) -- a single wheel event
    // could as much as halve or double the view, which on a trackpad's usual burst of
    // events per swipe added up to "jumps quickly to a very small bit" long before the
    // gesture felt finished. Both the per-event ceiling and the dy sensitivity are gentler
    // now (0.8/1.25 is an exact inverse pair, same idea as the keyboard zoom step).
    const double factor = juce::jlimit(0.8, 1.25, std::exp(-dy * 0.6));
    zoomToward(factor, wheelGestureAnchorX);
}

void WaveformDisplay::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // This fires on every selection/playhead change too (setSelection broadcasts), so only
    // rebuild the (O(samples)) waveform path when the audio content or view range actually
    // changed -- otherwise a selection drag would rescan the whole buffer every message loop.
    const bool contentChanged = refitViewIfContentChanged();

    const int64_t maxSpan = maxViewSpan();
    const bool viewBad = viewEnd <= viewStart || viewEnd > maxSpan;
    if (viewBad)
    {
        viewStart = 0;
        viewEnd = juce::jmax((int64_t) 1, maxSpan);
    }

    if (viewBad || contentChanged)
        rebuildWaveformPath();
    repaint();
}
