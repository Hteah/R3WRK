#pragma once
#include <JuceHeader.h>

/** Offline time-stretch / pitch-shift using librubberband. */
namespace TimeStretchEngine
{
    // stretchRatio: 1.0 = unchanged length, 2.0 = twice as long, 0.5 = half as long.
    // pitchSemitones: 0 = unchanged pitch, +/- semitones to transpose.
    // Returns an empty buffer on failure.
    juce::AudioBuffer<float> process(const juce::AudioBuffer<float>& input, double sampleRate,
                                      double stretchRatio, double pitchSemitones);
}
