#pragma once
#include <JuceHeader.h>
#include <cmath>

/**
    A single free-running LFO -- phase accumulator + one of five shapes, banded rate (slow /
    normal / audio) so one knob can cover minutes-long cycles up to fast wobble without a single
    log sweep being unusable at either end. Deliberately not juce::dsp (matches BiquadFilter.h's
    reasoning: plain doubles, no allocation, no locks -- safe to tick from the audio thread).

    Ticked once per BLOCK, not per sample (see PluginProcessor::tickLfos()) -- the same update
    granularity the filter and gain knobs already use elsewhere in this file. That means the
    "audio" rate band currently sounds like a fast wobble bounded by the host's block size, not
    true sample-accurate audio-rate modulation (real AM/FM character) -- a known limitation,
    fine for now, worth revisiting once there's a reason to make Gain modulation sample-accurate.
*/
namespace r3wrk
{
    enum class LfoShape { sine = 0, square = 1, triangle = 2, saw = 3, sampleHold = 4 };
    enum class LfoRateRange { slow = 0, normal = 1, audio = 2 };

    // What an LFO can steer. Pitch/Speed (the real-time RubberBand stretch engine) are
    // deliberately excluded -- that pipeline has its own history of click-storm bugs; modulating
    // it is a later, isolated pass. Gain is the Standalone-only output knob (harmless, just
    // inert, in a plugin build where it's never turned).
    enum class ModTarget
    {
        none = 0,
        filterBase, filterWidth, filterHpQ, filterLpQ,
        gain
    };
    constexpr int kNumModTargets = (int) ModTarget::gain + 1;

    // 0..1 knob position -> Hz, banded so "very slow (minutes) to audio speeds" (the brief)
    // fits under one knob without a single sweep being unusably coarse at one end.
    inline double lfoRateHz(double rate01, LfoRateRange range) noexcept
    {
        rate01 = juce::jlimit(0.0, 1.0, rate01);
        double loHz, hiHz;
        switch (range)
        {
            case LfoRateRange::slow:  loHz = 1.0 / 600.0; hiHz = 1.0;    break;   // 10 min .. 1 s / cycle
            case LfoRateRange::audio: loHz = 20.0;         hiHz = 5000.0; break;
            case LfoRateRange::normal:
            default:                 loHz = 0.05;         hiHz = 20.0;  break;
        }
        return loHz * std::pow(hiHz / loHz, rate01);   // exponential sweep -- even feel across the band
    }

    struct LfoModule
    {
        double phase   = 0.0;    // 0..1, audio-thread-only
        double shHeld  = 0.0;    // current sample & hold value
        bool   shPrimed = false;
        uint32_t rng = 0x9E3779B9u;

        void reset() noexcept { phase = 0.0; shPrimed = false; }

        double nextRandom() noexcept
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;   // xorshift32
            return (double) (rng & 0xFFFFFFu) / (double) 0xFFFFFFu * 2.0 - 1.0;
        }

        static double evaluate(double ph, LfoShape shape, double heldValue) noexcept
        {
            switch (shape)
            {
                case LfoShape::sine:     return std::sin(ph * juce::MathConstants<double>::twoPi);
                case LfoShape::square:   return ph < 0.5 ? 1.0 : -1.0;
                case LfoShape::triangle: return 4.0 * std::abs(ph - std::floor(ph + 0.5)) - 1.0;
                case LfoShape::saw:      return 2.0 * ph - 1.0;
                case LfoShape::sampleHold: default: return heldValue;
            }
        }

        // Advances by one block's worth of phase and returns the resulting bipolar (-1..+1)
        // output. `rateHz` should already be the banded Hz from lfoRateHz().
        double tickBlock(double rateHz, LfoShape shape, double sampleRate, int numSamples) noexcept
        {
            sampleRate = juce::jmax(1.0, sampleRate);
            const double inc = rateHz * (double) juce::jmax(0, numSamples) / sampleRate;

            phase += inc;
            const bool wrapped = phase >= 1.0;
            if (wrapped)
                phase -= std::floor(phase);

            if (shape == LfoShape::sampleHold && (wrapped || ! shPrimed))
            {
                shHeld = nextRandom();
                shPrimed = true;
            }

            return evaluate(phase, shape, shHeld);
        }
    };
}
