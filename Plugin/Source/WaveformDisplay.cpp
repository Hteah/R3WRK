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

    const bool wasFullView = viewStart <= 0 && viewEnd >= lastKnownTotal;
    lastBufferVersion = version;
    lastKnownTotal = document.getNumSamples();

    if (wasFullView)
    {
        viewStart = 0;
        viewEnd = juce::jmax((int64_t) 1, lastKnownTotal);
    }
    return true;
}

void WaveformDisplay::timerCallback()
{
    // Rebuild whenever the audio content changed, or the view range is stale / degenerate,
    // or the component was resized before the buffer existed. Cheaper than trusting only the
    // async ChangeBroadcaster, and self-heals any ordering race on first load.
    const bool contentChanged = refitViewIfContentChanged();

    const int64_t total = document.getNumSamples();
    const bool viewBad = viewEnd <= viewStart || viewEnd > total || viewStart >= juce::jmax((int64_t) 1, total);
    const double timeScale = document.getTimeScale();

    if (contentChanged || viewBad
        || getWidth() != lastPathWidth || getHeight() != lastPathHeight
        || std::abs(timeScale - lastTimeScale) > 1.0e-9)
    {
        lastTimeScale = timeScale;
        if (viewBad)   // refitViewIfContentChanged() already handled the "was showing everything" case
        {
            viewStart = 0;
            viewEnd = juce::jmax((int64_t) 1, total);
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
// more of them fit, leaving blank space -- the sample looks shorter.
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

// Sieve-style editor zoom: `spanFactor` multiplies the visible span (<1 = zoom in).
//   - pointer near a selection bracket -> pin that bracket and zoom into it (no span limit),
//     so the edge you're pointing at becomes the focus;
//   - pointer elsewhere, zooming in, selection still narrower than the view -> frame the
//     whole selection so the wheel pulls you into it;
//   - otherwise -> keep the sample under the pointer fixed.
void WaveformDisplay::zoomToward(double spanFactor, float pointerX)
{
    const int64_t total = document.getNumSamples();
    if (total <= 0)
        return;

    spanFactor = juce::jlimit(0.5, 2.0, spanFactor);
    const int64_t curLen = juce::jmax((int64_t) 1, viewEnd - viewStart);
    const int64_t newLen = (int64_t) juce::jlimit(16.0, (double) total, (double) curLen * spanFactor);
    const double timeScale = juce::jmax(0.0001, document.getTimeScale());

    // anchorFrac is a fraction of the pixel width; convert it to a raw-sample offset via
    // the same timeScale-aware density xToSample()/sampleToX() use, so the anchor sample
    // stays pinned under the pointer even when the waveform is visually stretched.
    auto applyView = [&](int64_t anchorSample, double anchorFrac)
    {
        int64_t newStart = anchorSample - (int64_t) (anchorFrac * (double) newLen / timeScale);
        newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, total - newLen), newStart);
        viewStart = newStart;
        viewEnd = newStart + newLen;
        rebuildWaveformPath();
        repaint();
    };

    if (document.hasSelection())
    {
        const float sx = sampleToX(document.getSelectionStart());
        const float ex = sampleToX(document.getSelectionEnd());
        const float dStart = std::abs(pointerX - sx);
        const float dEnd   = std::abs(pointerX - ex);
        constexpr float bracketGrabPx = 12.0f;

        // Near a bracket: lock onto that edge and zoom into it, keeping it comfortably in
        // view (nudged off the very edges of the component). No selection-span clamp here,
        // so the wheel can take you right down onto the sample.
        if (juce::jmin(dStart, dEnd) <= bracketGrabPx)
        {
            const int64_t edge = (dStart <= dEnd) ? document.getSelectionStart()
                                                  : document.getSelectionEnd();
            const double frac = juce::jlimit(0.15, 0.85,
                                             (double) pointerX / (double) juce::jmax(1, getWidth()));
            applyView(edge, frac);
            return;
        }

        // Not near an edge: on zoom-in, frame the whole selection while it's still narrower
        // than the view.
        if (spanFactor < 1.0)
        {
            const int64_t selLen = document.getSelectionEnd() - document.getSelectionStart();
            if (selLen > 0 && curLen > selLen)
            {
                const int64_t framedLen = juce::jmax((int64_t) 16, juce::jmin(newLen, selLen + selLen / 5));
                const int64_t mid = (document.getSelectionStart() + document.getSelectionEnd()) / 2;
                int64_t newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, total - framedLen),
                                                mid - framedLen / 2);
                viewStart = newStart;
                viewEnd = newStart + framedLen;
                rebuildWaveformPath();
                repaint();
                return;
            }
        }
    }

    const double frac = juce::jlimit(0.0, 1.0, (double) pointerX / (double) juce::jmax(1, getWidth()));
    applyView(xToSample(pointerX), frac);
}

void WaveformDisplay::panByPixels(float dxPixels)
{
    const int64_t total = document.getNumSamples();
    const int64_t len = viewEnd - viewStart;
    if (len <= 0 || len >= total)
        return;

    const double framesPerPixel = (double) len / (double) juce::jmax(1, getWidth())
                                 / juce::jmax(0.0001, document.getTimeScale());
    int64_t newStart = viewStart - (int64_t) (dxPixels * framesPerPixel);
    newStart = juce::jlimit((int64_t) 0, total - len, newStart);
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
    const double samplesPerPixel = (double) rangeLen / (double) w
                                  / juce::jmax(0.0001, document.getTimeScale());

    // Deep zoom (fewer than one peak bin per pixel): copy just the actually-visible span
    // (w * samplesPerPixel raw samples -- bounded even at extreme Stretch, unlike the full
    // [viewStart,viewEnd) range) out from under the lock once, then read it per-sample below.
    juce::AudioBuffer<float> raw;
    int64_t rawStart = 0;
    if (samplesPerPixel < (double) peakBinSize)
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

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
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

    const double factor = juce::jlimit(0.5, 2.0, std::exp(-dy * 1.4));   // dy>0 (wheel up) => zoom in
    zoomToward(factor, (float) e.x);
}

void WaveformDisplay::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // This fires on every selection/playhead change too (setSelection broadcasts), so only
    // rebuild the (O(samples)) waveform path when the audio content or view range actually
    // changed -- otherwise a selection drag would rescan the whole buffer every message loop.
    const bool contentChanged = refitViewIfContentChanged();

    const bool viewBad = viewEnd <= viewStart || viewEnd > document.getNumSamples();
    if (viewBad)
    {
        viewStart = 0;
        viewEnd = juce::jmax((int64_t) 1, document.getNumSamples());
    }

    if (viewBad || contentChanged)
        rebuildWaveformPath();
    repaint();
}
