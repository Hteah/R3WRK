#include "TimeRuler.h"
#include <cmath>

namespace
{
    juce::String formatTick(double seconds, bool subSecond)
    {
        seconds = juce::jmax(0.0, seconds);
        const int mins = (int) (seconds / 60.0);
        const double secs = seconds - mins * 60.0;
        return subSecond ? juce::String::formatted("%d:%06.3f", mins, secs)
                         : juce::String::formatted("%d:%02d", mins, (int) (secs + 0.5));
    }
}

TimeRuler::TimeRuler(const WaveformDisplay& w, const AudioDocument& d)
    : waveform(w), document(d)
{
    setInterceptsMouseClicks(false, false);
    startTimerHz(30);
}

TimeRuler::~TimeRuler() = default;

// Smallest "nice" step (1/2/5 x 10^n, with musical-ish values above 1 s) that keeps
// labels at least ~80 px apart.
double TimeRuler::niceStepSeconds(double spanSeconds, double widthPx)
{
    static const double steps[] = {
        0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
        1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0
    };
    const double pxPerSec = widthPx / juce::jmax(1.0e-9, spanSeconds);
    const double rawStep  = 80.0 / juce::jmax(1.0e-9, pxPerSec);
    for (double s : steps)
        if (s >= rawStep)
            return s;
    return steps[(sizeof(steps) / sizeof(steps[0])) - 1];
}

void TimeRuler::paint(juce::Graphics& g)
{
    const auto& pal = theme->palette();
    const juce::Colour kInk = pal.screenText;   // ruler sits on panelBg ("the screen"), not windowBg

    const float w = (float) getWidth();
    const float h = (float) getHeight();
    g.fillAll(pal.panelBg);
    g.setColour(pal.gridLine);
    g.drawHorizontalLine(0, 0.0f, w);

    if (w <= 1.0f || h <= 2.0f)
        return;

    const double sr = document.getSampleRate() > 0.0 ? document.getSampleRate() : 44100.0;
    const int64_t total = document.getNumSamples();
    if (total <= 0)
        return;

    int64_t vs = waveform.getViewStart();
    int64_t ve = waveform.getViewEnd();
    if (ve <= vs) { vs = 0; ve = total; }

    const double spanSec = (double) (ve - vs) / sr;
    if (spanSec <= 0.0)
        return;

    const double step     = niceStepSeconds(spanSec, (double) w);
    const bool   sub       = step < 1.0;
    const double pxPerSec  = (double) w / spanSec;
    const double startSec  = (double) vs / sr;
    const double endSec     = startSec + spanSec;

    const float majorTickH = juce::jmin(9.0f, h * 0.45f);
    const float minorTickH = juce::jmin(4.0f, h * 0.22f);
    const float labelY      = majorTickH + 1.0f;

    // minor ticks (step / 5), unlabelled
    const double minorStep = step / 5.0;
    for (double mt = std::ceil(startSec / minorStep) * minorStep; mt < endSec; mt += minorStep)
    {
        const float x = (float) ((mt - startSec) * pxPerSec);
        g.setColour(kInk.withAlpha(0.12f));
        g.drawVerticalLine((int) x, 0.0f, minorTickH);
    }

    // major ticks + labels
    g.setFont(juce::FontOptions(10.0f));
    for (double t = std::ceil(startSec / step) * step; t < endSec + 1.0e-9; t += step)
    {
        const float x = (float) ((t - startSec) * pxPerSec);
        g.setColour(kInk.withAlpha(0.30f));
        g.drawVerticalLine((int) x, 0.0f, majorTickH);
        g.setColour(kInk.withAlpha(0.55f));
        g.drawText(formatTick(t, sub), (int) x + 3, (int) labelY, 90, (int) (h - labelY),
                   juce::Justification::centredLeft, false);
    }

    // Playhead marker. Placed via WaveformDisplay::sampleToX() (not this ruler's own
    // seconds-based math above) because the playhead is a raw sample position, and with
    // Speed/Stretch off-centre the raw-sample-to-pixel density differs from the
    // output-time-to-pixel density the tick labels use -- see WaveformDisplay::xToSample().
    const float x = waveform.sampleToX(document.playhead.load());
    if (x >= -4.0f && x <= w + 4.0f)
    {
        juce::Path tri;
        tri.addTriangle(x - 4.0f, 0.0f, x + 4.0f, 0.0f, x, 7.0f);
        g.setColour(pal.playhead);
        g.fillPath(tri);
    }
}
