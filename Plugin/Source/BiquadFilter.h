#pragma once
#include <JuceHeader.h>
#include <cmath>

/**
    A tiny hand-rolled RBJ biquad (transposed direct-form II) for the knob-row multi-mode
    filter. Deliberately NOT juce::dsp -- AudioDocument.cpp bakes the same filter into
    Save/Export and is compiled into the headless smoke-test target, which doesn't link
    juce_dsp. So the exact same coefficient math is shared by the real-time playback path
    (PluginProcessor) and the offline bake (AudioDocument::renderWithPlaybackKnobs).

    Modes match AudioDocument::filterMode: 0 = off, 1 = low-pass, 2 = high-pass,
    3 = band-pass (constant-skirt, ~0 dB peak), 4 = notch. 12 dB/oct.
*/
namespace r3wrk
{
    enum FilterMode { filterOff = 0, filterLP = 1, filterHP = 2, filterBP = 3, filterNotch = 4 };

    // The Res knob (0..1) -> filter Q. Exponential so resonance gets dramatic near the top:
    // 0 -> 0.5 (gentle), 0.5 -> ~2.4, 1.0 -> 12.
    inline double filterResonanceToQ (double res01)
    {
        return 0.5 * std::pow (24.0, juce::jlimit (0.0, 1.0, res01));
    }

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // a0 normalised to 1
        double z1 = 0.0, z2 = 0.0;

        void reset() noexcept { z1 = z2 = 0.0; }

        // RBJ cookbook coefficients. `fc` is clamped to a sane range for `fs`.
        void setCoeffs (int mode, double fc, double q, double fs) noexcept
        {
            fs = juce::jmax (1.0, fs);
            fc = juce::jlimit (10.0, fs * 0.49, fc);
            q  = juce::jmax (0.05, q);

            const double w0    = 2.0 * juce::MathConstants<double>::pi * fc / fs;
            const double cosw0 = std::cos (w0);
            const double sinw0 = std::sin (w0);
            const double alpha = sinw0 / (2.0 * q);

            double B0, B1, B2, A0, A1, A2;
            A0 = 1.0 + alpha;
            A1 = -2.0 * cosw0;
            A2 = 1.0 - alpha;

            switch (mode)
            {
                case filterHP:
                    B0 =  (1.0 + cosw0) * 0.5;
                    B1 = -(1.0 + cosw0);
                    B2 =  (1.0 + cosw0) * 0.5;
                    break;
                case filterBP:                 // constant skirt gain, peak ~= Q  -> normalise to 0 dB
                    B0 =  alpha;
                    B1 =  0.0;
                    B2 = -alpha;
                    break;
                case filterNotch:
                    B0 =  1.0;
                    B1 = -2.0 * cosw0;
                    B2 =  1.0;
                    break;
                case filterLP:
                default:
                    B0 = (1.0 - cosw0) * 0.5;
                    B1 =  1.0 - cosw0;
                    B2 = (1.0 - cosw0) * 0.5;
                    break;
            }

            b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0;
            a1 = A1 / A0; a2 = A2 / A0;
        }

        inline float processSample (float x) noexcept
        {
            const double xn = (double) x;
            const double y  = b0 * xn + z1;
            z1 = b1 * xn - a1 * y + z2;
            z2 = b2 * xn - a2 * y;
            return (float) y;
        }

        void processBlock (float* data, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
                data[i] = processSample (data[i]);
        }
    };
}
