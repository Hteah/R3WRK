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

// Follow-playhead (EditorToolbar's Follow toggle -> document.followPlayheadEnabled): while
// playing and zoomed in past the whole clip, slide the view each frame so the playhead sits
// at the centre and the waveform scrolls under it. Clamps at the clip edges (the playhead
// drifts off-centre there), and does nothing when the whole clip is already visible. Mirrors
// Sieve's editor. A manual pan/zoom still works -- the next frame just re-centres.
void WaveformDisplay::followPlayheadIfNeeded()
{
    const bool active = document.followPlayheadEnabled
                     && document.isPlaying.load()
                     && ! document.isScrubbing.load()
                     && ! document.isRecording.load();

    const int64_t span = viewEnd - viewStart;
    if (! active || span <= 0 || span >= maxViewSpan())   // not following / zoomed all the way out
    {
        followViewAnchorSample = -1;   // paint()/TimeRuler go back to the live playhead
        return;
    }

    // Read the playhead ONCE and both scroll the view *and* record it as the value to draw the
    // playhead line from (playheadDrawSample()). The audio thread advances document.playhead
    // continuously, so if paint() re-read it a few ms later the line would land off-centre by
    // (samples advanced / samples-per-pixel) -- hundreds of px when zoomed in on a long file --
    // then snap back next tick. That mismatch was the "tapehead shake".
    const int64_t playheadNow = document.playhead.load();
    followViewAnchorSample = playheadNow;

    // The playhead is a raw sample index; viewStart/viewEnd are raw sample bounds. sampleToX()
    // divides samples-per-pixel by getTimeScale(), so the raw span actually across the width is
    // span/timeScale -- hence the half-offset is span/(2*timeScale), which is span/2 at identity.
    const double timeScale = juce::jmax(0.0001, document.getTimeScale());
    const int64_t half   = (int64_t) std::llround((double) span / (2.0 * timeScale));
    const int64_t maxStart = juce::jmax((int64_t) 0, maxViewSpan() - span);
    const int64_t target = juce::jlimit((int64_t) 0, maxStart, playheadNow - half);

    if (target == viewStart)
        return;

    viewStart = target;
    viewEnd   = target + span;
    rebuildWaveformPath();
}

int64_t WaveformDisplay::playheadDrawSample() const
{
    return followViewAnchorSample >= 0 ? followViewAnchorSample : document.playhead.load();
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

    if (contentChanged || timeScaleChanged || viewBad
        || getWidth() != lastPathWidth || getHeight() != lastPathHeight)
    {
        if (viewBad)   // refits above already handled the "was showing everything" cases
        {
            viewStart = 0;
            viewEnd = juce::jmax((int64_t) 1, maxSpan);
        }
        rebuildWaveformPath();
    }

    followPlayheadIfNeeded();   // after the view is known-good; rebuilds the path itself if it scrolls
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

// Keyboard zoom (⌘+/⌘-, see PluginEditor::keyPressed). Anchored at the centre of the view,
// matching Sieve's editor: with a selection active, zoomToward()'s Phase 1 ignores the anchor
// and frames the selection centred anyway, so ⌘+ walks you into the selection hands-free;
// with no selection it just zooms about the middle. The step is a gentle inverse pair (0.9,
// 1/0.9) -- much smaller than a hard/fast wheel notch -- so repeated presses zoom in
// comfortably small increments and in-then-out lands back on the same span.
static constexpr double keyboardZoomStep = 0.9;
void WaveformDisplay::zoomIn()  { zoomToward(keyboardZoomStep,       (float) getWidth() * 0.5f); }
void WaveformDisplay::zoomOut() { zoomToward(1.0 / keyboardZoomStep, (float) getWidth() * 0.5f); }

void WaveformDisplay::keyboardScroll(int dir, bool bigStep)
{
    const int64_t n = document.getNumSamples();
    if (n <= 0 || dir == 0)
        return;

    // Step is a fraction of what's actually visible, so it feels the same at any zoom. The
    // raw span across the width is (viewEnd-viewStart)/timeScale (sampleToX divides s/px by it).
    const double  timeScale  = juce::jmax(0.0001, document.getTimeScale());
    const int64_t span       = viewEnd - viewStart;
    const int64_t visibleRaw = (int64_t) ((double) span / timeScale);
    const int64_t delta = (int64_t) dir * juce::jmax((int64_t) 1,
                              (int64_t) ((double) visibleRaw * (bigStep ? 0.5 : 1.0 / 20.0)));

    bool moved = false;

    // Scroll the view (only meaningful when zoomed in past the whole clip).
    const bool canScroll = span > 0 && span < maxViewSpan();
    if (canScroll)
    {
        const int64_t maxStart = juce::jmax((int64_t) 0, maxViewSpan() - span);
        const int64_t newStart = juce::jlimit((int64_t) 0, maxStart, viewStart + delta);
        if (newStart != viewStart)
        {
            viewStart = newStart;
            viewEnd   = newStart + span;
            moved = true;
        }
    }

    // While stopped, the playhead moves with the view (stays put on screen when scrolling,
    // and still moves when zoomed all the way out). During playback it's driven by the audio
    // thread, so leave it alone -- the arrows just pan the view then.
    if (! document.isPlaying.load())
    {
        const int64_t ph = juce::jlimit((int64_t) 0, n, document.playhead.load() + delta);
        if (ph != document.playhead.load())
        {
            document.playhead = ph;
            moved = true;
        }
    }

    if (moved)
    {
        if (canScroll)
            rebuildWaveformPath();
        document.notifyChanged();
    }
}

// Sieve-style editor zoom -- ported from Sieve's EditorWaveformView.zoom() so it feels the
// same. `spanFactor` multiplies the visible span (<1 = zoom in, >1 = zoom out).
//
// Zooming IN with a selection has two phases:
//   Phase 1 -- the view is still wider than the selection: ignore the pointer, frame the
//     selection centred, and let each notch tighten the view toward it (never past 1.2x the
//     selection's width). This is the "it locks onto my selection and pulls me in" feel.
//   Phase 2 -- the view is now inside the selection: zoom toward the pointer, keeping the
//     sample under it fixed; but if the pointer is hugging one side and that selection
//     bracket has scrolled off that edge, anchor on the bracket so it comes back into view.
// Zooming OUT, and zooming with no selection: plain pointer-anchored zoom -- the sample
// under the pointer stays put. (Sieve does nothing special for zoom-out; earlier R3WRK
// builds pinned a selection edge here, which is what made "zoom out always drifts to one
// side" -- removed.)
void WaveformDisplay::zoomToward(double spanFactor, float pointerX)
{
    const int64_t total = document.getNumSamples();
    if (total <= 0)
        return;

    spanFactor = juce::jlimit(0.2, 5.0, spanFactor);

    const int64_t maxSpan   = maxViewSpan();
    const int64_t curLen    = juce::jmax((int64_t) 1, viewEnd - viewStart);
    const double  timeScale = juce::jmax(0.0001, document.getTimeScale());
    const double  width     = (double) juce::jmax(1, getWidth());
    constexpr int64_t minSpan = 16;

    int64_t newLen = juce::jlimit(minSpan, maxSpan,
                                  (int64_t) std::llround((double) curLen * spanFactor));

    // Commit a view of `newLen` scaled samples with `anchorSample` (a raw sample) pinned
    // under `anchorFrac` of the pixel width. The raw-samples-per-pixel density divides by
    // timeScale, same as xToSample()/sampleToX(), so the anchor holds even when the
    // Speed/Pitch/Stretch knobs have the waveform visually stretched.
    auto applyView = [&](int64_t anchorSample, double anchorFrac)
    {
        int64_t newStart = anchorSample
                         - (int64_t) std::llround(anchorFrac * (double) newLen / timeScale);
        newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, maxSpan - newLen), newStart);
        viewStart = newStart;
        viewEnd   = newStart + newLen;
        rebuildWaveformPath();
        repaint();
    };

    if (document.hasSelection())
    {
        const int64_t selStart = document.getSelectionStart();
        const int64_t selEnd   = document.getSelectionEnd();
        const int64_t selLen   = juce::jmax((int64_t) 1, selEnd - selStart);
        const int64_t selMid   = (selStart + selEnd) / 2;

        // Raw samples visible across the width right now (curLen is a scaled span).
        const int64_t visibleRawNow = (int64_t) ((double) curLen / timeScale);

        if (spanFactor < 1.0)   // ---- zooming IN ----
        {
            if (visibleRawNow > selLen)
            {
                // Phase 1: frame the selection, centred, tightening toward 1.2x its width.
                const int64_t framedRaw = juce::jmax(minSpan, selLen * 6 / 5);
                newLen = juce::jlimit(minSpan, maxSpan,
                                      juce::jmin(newLen,
                                                 (int64_t) std::llround((double) framedRaw * timeScale)));
                applyView(selMid, 0.5);
                return;
            }

            // Phase 2: zoom toward the pointer inside the selection, pulling an off-screen
            // bracket back into view when the pointer hugs that side.
            const double     frac  = juce::jlimit(0.0, 1.0, (double) pointerX / width);
            constexpr double edge  = 0.22;
            const int64_t    leftRaw  = xToSample(0.0f);
            const int64_t    rightRaw = xToSample((float) width);

            int64_t anchorSample;
            if (frac <= edge && selStart < leftRaw)
                anchorSample = selStart;
            else if (frac >= 1.0 - edge && selEnd > rightRaw)
                anchorSample = selEnd;
            else
                anchorSample = xToSample(pointerX);

            applyView(anchorSample, frac);
            return;
        }

        // ---- zooming OUT ----
        // Mirror of Phase 1: while the view is still framed roughly on the selection, back
        // away keeping it centred, so a zoom-out re-frames the selection instead of drifting
        // off toward whichever side the pointer happened to sit on (which, compounded over a
        // trackpad's burst of wheel events, was throwing the view into a huge lopsided
        // zoom-out). Only once the view is several times wider than the selection -- you're
        // clearly looking at broader context now -- does it release to free pointer-anchored
        // zoom-out.
        constexpr int64_t stickySelectionSpans = 3;
        if (visibleRawNow < selLen * stickySelectionSpans)
        {
            applyView(selMid, 0.5);
            return;
        }
    }

    const double frac = juce::jlimit(0.0, 1.0, (double) pointerX / width);
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
        // Try-lock, never block: if processBlock is mid-callback holding this, keep the old
        // cache and let rebuildWaveformPath() retry next tick. A blocking lock here on the
        // message thread would stall the audio thread's own try-lock -> a dropped block -> click.
        const juce::CriticalSection::ScopedTryLockType sl(document.getLock());
        if (! sl.isLocked())
            return;
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
    waveformIsSampleLine = false;
    showSampleDots = false;
    sampleDots.clear();

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

    // When Speed/Pitch/Stretch spread the clip out (or squeeze it), the drawn shape is the
    // stored waveform rescaled to the new duration -- samplesPerPixel already folds in
    // getTimeScale() -- plus a "transient smear": a box-blur of the per-pixel envelope whose
    // radius grows with how far the ratio is from 1x, so a stretched view rounds sharp hits
    // off instead of just drawing them wider. (A true RubberBand render of the whole clip was
    // tried -- WaveformStretchPreview, removed -- but at extreme ratios it's unusably slow, ~14
    // minutes of output audio for a 16 s clip at 50x, and its necessarily coarse peak cache
    // drew as terraces when zoomed in anyway.)
    const int smearRadius = juce::jlimit(0, 8,
                                juce::roundToInt(std::abs(std::log2(timeScale)) * 1.4));

    // Deep zoom (fewer than one peak-cache bin per pixel): draw from a copy of the real
    // samples. `rawCache` holds a span WIDER than the viewport and is kept across rebuilds --
    // it's only refreshed when the view has scrolled past its margins or the audio changed --
    // so a frame whose try-lock on getLock() loses to processBlock still has samples to draw
    // (before, a lock-miss frame fell back to the blocky bin cache, strobing the waveform
    // during follow-playhead playback). Try-lock only -- a blocking lock would beat
    // processBlock's try-lock -> dropped block -> crackle.
    if (samplesPerPixel < (double) peakBinSize)
    {
        const int64_t firstVis = juce::jlimit((int64_t) 0, total, viewStart);
        const int64_t visible  = (int64_t) (samplesPerPixel * (double) w) + 8;
        const int64_t lastVis  = juce::jlimit((int64_t) 0, total, firstVis + visible);

        const bool cacheCovers = rawCacheVersion == document.getBufferVersion()
                              && rawCache.getNumSamples() > 0
                              && rawCacheStart <= firstVis
                              && rawCacheStart + (int64_t) rawCache.getNumSamples() >= lastVis;

        if (! cacheCovers)
        {
            const juce::CriticalSection::ScopedTryLockType sl(document.getLock());
            if (sl.isLocked())
            {
                auto& src = document.getBuffer();
                const int64_t srcLen = (int64_t) src.getNumSamples();
                const int64_t margin = juce::jmax(visible, (int64_t) 8192) * 2;   // scroll slack each side
                const int64_t wantStart = juce::jlimit((int64_t) 0, srcLen, firstVis - margin);
                const int64_t wantLen   = juce::jmax((int64_t) 0,
                                              juce::jmin(srcLen - wantStart, (lastVis - wantStart) + margin));
                if (wantLen > 0 && src.getNumChannels() > 0)
                {
                    rawCache.setSize(src.getNumChannels(), (int) wantLen, false, false, true);
                    for (int ch = 0; ch < src.getNumChannels(); ++ch)
                        rawCache.copyFrom(ch, 0, src, ch, (int) wantStart, (int) wantLen);
                    rawCacheStart = wantStart;
                    rawCacheVersion = document.getBufferVersion();
                }
            }
        }
    }

    // The cache is usable this frame if it's for the current audio and starts at or before the
    // view. Per-pixel/per-sample loops below still bounds-check each index, so a cache that
    // only partially covers the right edge just leaves a sliver of bin-cache pixels there,
    // never a full strobe.
    const bool haveRaw = samplesPerPixel < (double) peakBinSize
                      && rawCacheVersion == document.getBufferVersion()
                      && rawCache.getNumSamples() > 0
                      && rawCacheStart <= juce::jmax((int64_t) 0, viewStart);
    const juce::AudioBuffer<float>& raw = rawCache;
    const int64_t rawStart = rawCacheStart;

    // Once there's more than a pixel per sample, a min/max envelope has nothing left to show --
    // it flattens into a sample-and-hold staircase. Draw a polyline through the actual sample
    // values instead (paint() strokes it), and past ~5 px/sample also mark each sample.
    waveformIsSampleLine = haveRaw && samplesPerPixel < 1.0;
    showSampleDots       = waveformIsSampleLine && samplesPerPixel < 0.2;
    if (waveformIsSampleLine)
        sampleDots.assign((size_t) numCh, {});

    if (waveformIsSampleLine)
    {
        const int64_t firstS = juce::jmax((int64_t) 0, viewStart);
        const int64_t lastS  = juce::jmin(total - 1,
                                          viewStart + (int64_t) std::ceil(samplesPerPixel * (double) w) + 1);
        for (int ch = 0; ch < numCh && ch < raw.getNumChannels(); ++ch)
        {
            const float* d = raw.getReadPointer(ch);
            const float laneMid  = (float) (ch * laneHeight) + (float) laneHeight * 0.5f;
            const float laneHalf = (float) laneHeight * 0.48f;

            juce::Path p;
            bool started = false;
            for (int64_t s = firstS; s <= lastS; ++s)
            {
                const int64_t idx = s - rawStart;
                if (idx < 0 || idx >= raw.getNumSamples())
                    continue;
                const float x = sampleToX(s);
                const float y = laneMid - juce::jlimit(-1.0f, 1.0f, d[idx]) * laneHalf;
                if (! started) { p.startNewSubPath(x, y); started = true; }
                else            p.lineTo(x, y);
                if (showSampleDots)
                    sampleDots[(size_t) ch].push_back({ x, y });
            }
            channelPaths.push_back(std::move(p));
        }
        return;
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
            const int64_t r0 = s0 - rawStart, r1 = s1 - rawStart;
            if (haveRaw && ch < raw.getNumChannels()
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
            mins[(size_t) x] = mn;
            maxs[(size_t) x] = mx;
        }

        // Transient smear: average each envelope point over +/- smearRadius pixels, so a
        // stretched view softens sharp hits rather than just stretching the spikes wider.
        if (smearRadius > 0 && w > 1)
        {
            const auto blur = [w, smearRadius] (std::vector<float>& a)
            {
                const std::vector<float> in (a);
                for (int x = 0; x < w; ++x)
                {
                    float acc = 0.0f;
                    const int lo = juce::jmax(0, x - smearRadius);
                    const int hi = juce::jmin(w - 1, x + smearRadius);
                    for (int k = lo; k <= hi; ++k)
                        acc += in[(size_t) k];
                    a[(size_t) x] = acc / (float) (hi - lo + 1);
                }
            };
            blur(mins);
            blur(maxs);
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
        const juce::CriticalSection::ScopedTryLockType sl(document.getLock());   // never block the audio thread
        if (! sl.isLocked())
            return;   // skip this frame's overlay; it'll draw on the next paint
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

    std::vector<float> mins((size_t) n), maxs((size_t) n);
    for (int ch = 0; ch < juce::jmin(numCh, raw.getNumChannels()); ++ch)
    {
        // Amplify only lands on the focused channel(s), so preview the others unchanged.
        const float gain = document.channelInFocus(ch) ? document.previewGainLinear : 1.0f;
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

    // Channel focus: the lane(s) not being worked on draw dimmed, so it's obvious the next
    // Amplify / Fade / etc. only lands on one side (see AudioDocument::channelFocus).
    const auto laneColour = [&](int ch)
    {
        return document.channelInFocus(ch) ? pal.waveform : pal.waveform.withMultipliedAlpha(0.28f);
    };

    if (waveformIsSampleLine)
    {
        const juce::PathStrokeType stroke(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        for (int ch = 0; ch < (int) channelPaths.size(); ++ch)
        {
            g.setColour(laneColour(ch));
            g.strokePath(channelPaths[(size_t) ch], stroke);
        }

        if (showSampleDots)
            for (int ch = 0; ch < (int) sampleDots.size(); ++ch)
            {
                g.setColour(laneColour(ch));
                for (auto& pt : sampleDots[(size_t) ch])
                    g.fillEllipse(pt.x - 2.0f, pt.y - 2.0f, 4.0f, 4.0f);
            }
    }
    else
    {
        for (int ch = 0; ch < (int) channelPaths.size(); ++ch)
        {
            g.setColour(laneColour(ch));
            g.fillPath(channelPaths[(size_t) ch]);
        }
    }

    g.setColour(pal.gridLine);
    for (int ch = 1; ch < numCh; ++ch)
        g.drawHorizontalLine(ch * laneHeight, 0.0f, (float) getWidth());

    paintSelectionPreview(g);

    // Selection: a wash, plus a bracket at each edge — a 2 px line with a small handle pill
    // at top and bottom (same as Sieve's editor). In the Slice tool the brackets are
    // suppressed: a right-click sets a selection spanning the audition slice, and its blue
    // edges are drawn identically to an (orange) slice marker -- landing right on one -- which
    // reads as "the marker turned blue and won't delete". Just the wash there.
    if (document.hasSelection())
    {
        const juce::Colour accent = pal.accent;
        const float h  = (float) getHeight();
        const float x0 = sampleToX(document.getSelectionStart());
        const float x1 = sampleToX(document.getSelectionEnd());

        g.setColour(accent.withAlpha(0.16f));
        g.fillRect(juce::Rectangle<float>(x0, 0.0f, juce::jmax(1.0f, x1 - x0), h));

        if (! document.sliceModeEnabled)
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

    // Slice markers (Slice tool): same 2px line + top/bottom handle pills as the selection
    // brackets, so they're the "similar size" the user asked for -- but in the loop-marker
    // colour rather than the selection accent, so the two don't read as the same thing.
    // Only drawn while the Slice tool is selected; turning it off hides them (they stay in
    // document.sliceMarkers / plugin state and come back when the tool is re-enabled) and the
    // mouse handlers above fall through to normal editing.
    if (document.sliceModeEnabled)
    {
        const auto& marks = document.getSliceMarkers();
        const float h = (float) getHeight();
        const juce::Colour sliceCol = pal.loopMarker;
        for (int64_t m : marks)
        {
            const float x = sampleToX(m);
            if (x < -6.0f || x > (float) getWidth() + 6.0f)
                continue;

            g.setColour(sliceCol.withAlpha(0.9f));
            g.fillRect(juce::Rectangle<float>(x - 1.0f, 0.0f, 2.0f, h));

            constexpr float hw = 5.0f, hh = 14.0f;
            g.setColour(sliceCol);
            for (float cy : { hh * 0.5f + 1.0f, h - hh * 0.5f - 1.0f })
                g.fillRoundedRectangle(x - hw * 0.5f, cy - hh * 0.5f, hw, hh, 2.0f);
        }
    }

    g.setColour(pal.playhead);
    g.drawVerticalLine((int) sampleToX(playheadDrawSample()), 0.0f, (float) getHeight());
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

    if (document.sliceModeEnabled)
    {
        sliceDragIndex     = sliceMarkerAtPixel((float) e.x, sliceMarkerHitPx);
        sliceDragMoved     = false;
        slicePressOnMarker = sliceDragIndex >= 0;
        sliceLeftPress     = e.mods.isLeftButtonDown() && ! e.mods.isPopupMenu();

        // A plain-left double-click: in the top/bottom handle band -> delete the nearest marker
        // (wide snap -- the y position already means "delete", so don't require a precise x);
        // in the body -> add a marker there (and grab it so a continued drag nudges it).
        if (sliceLeftPress && e.getNumberOfClicks() >= 2)
        {
            const bool inHandleBand = (float) e.y <= sliceHandleZonePx
                                   || (float) e.y >= (float) getHeight() - sliceHandleZonePx;

            if (inHandleBand)
            {
                const int idx = sliceMarkerAtPixel((float) e.x, sliceDeleteSnapPx);
                if (idx >= 0)
                {
                    document.removeSliceMarker(idx);
                    sliceDragIndex = -1;
                    slicePressOnMarker = false;
                }
            }
            else if (sliceDragIndex < 0)
            {
                document.addSliceMarker(xToSample((float) e.x));
                sliceDragIndex     = sliceMarkerAtPixel((float) e.x, sliceMarkerHitPx);   // grab for an immediate drag
                slicePressOnMarker = sliceDragIndex >= 0;
            }
        }
        return;
    }

    if (e.mods.isPopupMenu() && document.hasSelection())
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

int WaveformDisplay::sliceMarkerAtPixel(float x, float tolPx) const
{
    // Hit-test in pixel space, not sample space: a "6 px in samples" tolerance collapses to 1
    // sample when zoomed in past ~2 samples/px, and then whether a click lands on a marker
    // comes down to integer-truncation luck per marker -- which was the "some won't delete" bug.
    const auto& marks = document.getSliceMarkers();
    int best = -1;
    float bestDist = tolPx;
    for (int i = 0; i < (int) marks.size(); ++i)
    {
        const float d = std::abs(sampleToX(marks[(size_t) i]) - x);
        if (d <= bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void WaveformDisplay::playSliceAt(int64_t sample)
{
    const auto regions = document.getSliceRegions();
    for (auto r : regions)
    {
        if (sample >= r.getStart() && sample < r.getEnd())
        {
            document.setSelection(r.getStart(), r.getEnd());
            document.playhead = r.getStart();
            if (onSlicePlay) onSlicePlay();
            return;
        }
    }
    // no markers -> just play from the click point
    document.clearSelection();
    document.playhead = juce::jlimit((int64_t) 0, document.getNumSamples(), sample);
    if (onSlicePlay) onSlicePlay();
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (document.sliceModeEnabled)
    {
        if (sliceLeftPress && sliceDragIndex >= 0)   // plain left-drag a marker to move it
        {
            sliceDragIndex = document.moveSliceMarker(sliceDragIndex, xToSample((float) e.x));
            sliceDragMoved = true;
        }
        return;
    }

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
    if (document.sliceModeEnabled)
    {
        const bool wasClick = e.getDistanceFromDragStart() < 4 && ! sliceDragMoved;

        // e.getNumberOfClicks() >= 2 -> the 2nd release of a double-click; the action already
        // happened on the 1st release (or, for the left double, in mouseDown), so skip it.
        // Right / ctrl single-click plays the slice. Add + delete are both double-left-clicks
        // (handled in mouseDown); a plain left single-click does nothing.
        if (wasClick && e.getNumberOfClicks() < 2 && ! sliceLeftPress)
            playSliceAt(xToSample((float) e.x));

        sliceDragIndex = -1;
        slicePressOnMarker = false;
        sliceLeftPress = false;
        return;
    }

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
    if (document.sliceModeEnabled)
    {
        // Wider "you can delete here" zone in the top/bottom handle band, matching mouseDown's
        // snap; the tighter grab zone in the body.
        const bool inHandleBand = (float) e.y <= sliceHandleZonePx
                               || (float) e.y >= (float) getHeight() - sliceHandleZonePx;
        const bool overMarker = sliceMarkerAtPixel((float) e.x,
                                                   inHandleBand ? sliceDeleteSnapPx : sliceMarkerHitPx) >= 0;
        setMouseCursor(overMarker ? juce::MouseCursor::LeftRightResizeCursor    // drag to move / dbl-click a handle to delete
                                  : juce::MouseCursor::CrosshairCursor);         // dbl-click the body to add a marker
        return;
    }

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

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (document.sliceModeEnabled)
        return;   // slice add / play / delete are all handled in mouseDown / mouseUp now

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

    // dy>0 (wheel up) => zoom in. Gentle per-notch factor (0.8/1.25, an exact inverse pair,
    // same idea as the keyboard zoom step) so a trackpad's burst of events per swipe doesn't
    // lurch the view. The anchor is the live pointer x, read fresh on every notch -- Sieve's
    // editor does the same; zoomToward()'s Phase 1 selection-framing is what keeps a
    // zoom-in steady on the selection, so there's no gesture anchor to freeze here.
    const double factor = juce::jlimit(0.8, 1.25, std::exp(-dy * 0.6));
    zoomToward(factor, (float) e.x);
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
