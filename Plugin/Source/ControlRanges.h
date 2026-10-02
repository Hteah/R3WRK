#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

/**
    Value ranges (and skews) of the knobs whose travel isn't a plain 0..1 -- shared by the UI
    (KnobRow sets these on its sliders) and the MIDI CC dispatcher (MidiCcDispatcher.h), so a
    hardware knob at its centre lands exactly where the on-screen knob's centre is (Speed 1.00x,
    Stretch 1.00x, ...). Any knob not listed here is a plain 0..1 atomic on AudioDocument.
*/
namespace r3wrk::ranges
{
    inline juce::NormalisableRange<double> skewedAround(double lo, double hi, double centre)
    {
        juce::NormalisableRange<double> r(lo, hi);
        r.setSkewForCentre(centre);
        return r;
    }

    inline juce::NormalisableRange<double> pitch()   { return { AudioDocument::kMinPitch, AudioDocument::kMaxPitch }; }
    inline juce::NormalisableRange<double> speed()   { return skewedAround(AudioDocument::kMinSpeed, AudioDocument::kMaxSpeed, 1.0); }
    inline juce::NormalisableRange<double> stretch() { return skewedAround(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, 1.0); }
    inline juce::NormalisableRange<double> filterBase() { return skewedAround(0.0, 1.0, 0.35); }   // ~log Hz feel
    inline juce::NormalisableRange<double> gainDb()  { return { AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb }; }
    inline juce::NormalisableRange<double> overdubLevel() { return skewedAround(0.0, 2.0, 0.5); }
    inline juce::NormalisableRange<double> loopCrossfadeMs() { return { 0.0, 50.0 }; }
    inline juce::NormalisableRange<double> autoRecordThresholdDb() { return { -60.0, 0.0 }; }
}
