#pragma once
#include <JuceHeader.h>

/** Offline speed / time-stretch / pitch, using R3WRK's own lofi engine (LofiStretch.h) --
    the same code the real-time playback path runs, so a baked Save/Export sounds like what
    you heard. */
namespace TimeStretchEngine
{
    // speed: tape varispeed (pitch and time together), 1 = unchanged.
    // stretch: Paulstretch time dilation, 1 = unchanged length, 2 = twice as long.
    // pitchSemitones: lofi granular pitch, 0 = unchanged.
    // Output length is input length * stretch / speed. Returns an empty buffer on failure.
    juce::AudioBuffer<float> process(const juce::AudioBuffer<float>& input, double sampleRate,
                                     double speed, double stretch, double pitchSemitones);

    // Stretch + pitch only (speed 1) -- the destructive Time Stretch/Pitch dialog.
    inline juce::AudioBuffer<float> process(const juce::AudioBuffer<float>& input, double sampleRate,
                                            double stretchRatio, double pitchSemitones)
    {
        return process(input, sampleRate, 1.0, stretchRatio, pitchSemitones);
    }
}
