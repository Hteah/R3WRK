#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "WaveformDisplay.h"

/**
    A thin time ruler drawn under the waveform. It reads the waveform's current
    view range every frame so its ticks line up with whatever is on screen, the
    same way Sieve's audio-editor timeline does. Purely decorative — no mouse.
*/
class TimeRuler : public juce::Component,
                  private juce::Timer
{
public:
    TimeRuler(const WaveformDisplay& waveform, const AudioDocument& document);
    ~TimeRuler() override;

    void paint(juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }
    static double niceStepSeconds(double spanSeconds, double widthPx);

    const WaveformDisplay& waveform;
    const AudioDocument& document;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimeRuler)
};
