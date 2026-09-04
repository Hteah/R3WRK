#include "WaveformDisplay.h"

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

    if (document.hasSelection())
    {
        float x0 = sampleToX(document.getSelectionStart());
        float x1 = sampleToX(document.getSelectionEnd());
        g.setColour(juce::Colours::deepskyblue.withAlpha(0.22f));
        g.fillRect(juce::Rectangle<float>(x0, 0.0f, x1 - x0, (float) getHeight()));
    }

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

    if (document.loopEnabled && document.loopEnd > document.loopStart)
    {
        g.setColour(juce::Colours::orange.withAlpha(0.9f));
        g.drawVerticalLine((int) sampleToX(document.loopStart), 0.0f, (float) getHeight());
        g.drawVerticalLine((int) sampleToX(document.loopEnd), 0.0f, (float) getHeight());
    }

    g.setColour(juce::Colours::red);
    g.drawVerticalLine((int) sampleToX(document.playhead.load()), 0.0f, (float) getHeight());
}

void WaveformDisplay::mouseDown(const juce::MouseEvent& e)
{
    dragStartSample = xToSample((float) e.x);
    document.playhead = dragStartSample;
    document.setSelection(dragStartSample, dragStartSample);
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    document.setSelection(dragStartSample, xToSample((float) e.x));
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent&)
{
    document.setSelection(0, document.getNumSamples());
}

void WaveformDisplay::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy(wheel.deltaY > 0 ? 0.8 : 1.25, xToSample((float) e.x));
    }
    else
    {
        int64_t len = viewEnd - viewStart;
        int64_t shift = (int64_t) ((double) len * 0.15 * (wheel.deltaY > 0 ? -1.0 : 1.0));
        int64_t newStart = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, document.getNumSamples() - len), viewStart + shift);
        viewEnd = newStart + len;
        viewStart = newStart;
        rebuildWaveformPath();
        repaint();
    }
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
