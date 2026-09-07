#pragma once
#include <JuceHeader.h>
#include <cmath>

/**
    A tiny hand-rolled RBJ biquad (transposed direct-form II), plus r3wrk::MultiModeFilter --
    the Octatrack-style Base/Width multimode filter built from two of them. Deliberately NOT
    juce::dsp: AudioDocument.cpp bakes the same filter into Save/Export and is compiled into
    the headless smoke-test target, which doesn't link juce_dsp. So the exact same math is
    shared by the real-time playback path (PluginProcessor) and the offline bake
    (AudioDocument::renderWithPlaybackKnobs).

    Biquad modes: 0 = off, 1 = low-pass, 2 = high-pass, 3 = band-pass (constant-skirt,
    ~0 dB peak), 4 = notch. 12 dB/oct.
*/
namespace r3wrk
{
    enum FilterMode { filterOff = 0, filterLP = 1, filterHP = 2, filterBP = 3, filterNotch = 4 };

    // The Q knob (0..1) -> filter Q, per edge. Exponential so resonance gets dramatic near
    // the top: 0 -> 0.5 (gentle), 0.5 -> ~2.4, 1.0 -> 12.
    inline double filterResonanceToQ (double res01)
    {
        return 0.5 * std::pow (24.0, juce::jlimit (0.0, 1.0, res01));
    }

    // Filter edge position (0..1) <-> frequency (20 Hz .. 20 kHz), logarithmic. Both the
    // Base knob's readout and the old-state migration use these.
    inline double filterPosToHz (double pos01)
    {
        return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, pos01));
    }

    inline double filterHzToPos (double hz)
    {
        return juce::jlimit (0.0, 1.0, std::log (juce::jmax (20.0, hz) / 20.0) / std::log (1000.0));
    }

    // The two edge frequencies for a Base/Width pair: Base is the low edge (high-pass),
    // Base+Width the high edge (low-pass), clamped so Width past the top just pins the
    // low-pass wide open.
    inline void filterEdges (double base01, double width01, double& fLowEdge, double& fHighEdge)
    {
        base01  = juce::jlimit (0.0, 1.0, base01);
        width01 = juce::jlimit (0.0, 1.0, width01);
        fLowEdge  = filterPosToHz (base01);
        fHighEdge = filterPosToHz (juce::jmin (1.0, base01 + width01));
    }

    // "Engaged" == the filter would change the sound: at least one edge is doing something
    // audible (Base ~0 => no high-pass; Base+Width ~1 => no low-pass), OR Drive is up (drive
    // alone, with both edges open, is just a saturator). Otherwise callers skip it and don't
    // count it as a playback knob that forces the slow RubberBand / offline render path.
    inline bool filterEngaged (double base01, double width01, double drive01 = 0.0)
    {
        return base01 > 0.004 || (base01 + width01) < 0.996 || drive01 > 0.001;
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

    /**
        The Octatrack-style multimode filter: a 2-pole high-pass (the Base / low edge) and a
        2-pole low-pass (the Base+Width / high edge) in series, resonance (Q) on both. You dial
        the two edges of the passband directly instead of picking a mode:

            Base 0    + Width 1    -> both edges bypassed, wide open (no effect)
            Base 0    + Width mid  -> low-pass
            Base mid  + Width 1    -> high-pass
            Base mid  + Width small-> resonant band-pass with a definable gap

        Each edge stage bypasses itself when it isn't doing anything, so an "open" filter is a
        true passthrough. A narrow, resonant band stacks two peaks and can get very loud -- a
        mild width/Q-dependent output trim keeps that musical rather than explosive.

        Drive (0..1) is a pre-filter saturator: input gain 1x..10x into a tanh, then partial
        makeup gain -- so cranking Drive adds harmonics (which the two edges then shape) more
        than it adds level. This is R3WRK's take on the Octatrack's filter DIST, which the OT
        manual describes only as "sets the headroom of the filter -- higher value = lower
        headroom" (i.e. drive harder into a fixed ceiling, ahead of the poles). The OT loses
        more level than this does; ride the Gain knob if you want it darker still.
    */
    struct MultiModeFilter
    {
        Biquad hp, lp;
        bool  hpOn = false, lpOn = false, driveOn = false;
        float outTrim = 1.0f, driveGain = 1.0f, driveMakeup = 1.0f;

        void reset() noexcept { hp.reset(); lp.reset(); }

        void setParams (double base01, double width01, double res01, double drive01, double fs) noexcept
        {
            base01  = juce::jlimit (0.0, 1.0, base01);
            width01 = juce::jlimit (0.0, 1.0, width01);
            res01   = juce::jlimit (0.0, 1.0, res01);
            drive01 = juce::jlimit (0.0, 1.0, drive01);

            double fLow, fHigh;
            filterEdges (base01, width01, fLow, fHigh);
            const double q = filterResonanceToQ (res01);

            hpOn = base01 > 0.004;
            lpOn = (base01 + width01) < 0.996 && fHigh < juce::jmax (1.0, fs) * 0.49;

            if (hpOn)
                hp.setCoeffs (filterHP, fLow, q, fs);
            if (lpOn)
                lp.setCoeffs (filterLP, juce::jmax (fHigh, fLow * 1.02), q, fs);

            // Only trims when the band is genuinely narrow *and* resonant; unity by width 0.5.
            outTrim = (hpOn && lpOn)
                ? (float) (1.0 / (1.0 + 3.0 * res01 * juce::jmax (0.0, 0.5 - width01)))
                : 1.0f;

            driveOn     = drive01 > 0.001;
            driveGain   = (float) (1.0 + drive01 * 9.0);
            driveMakeup = (float) std::pow ((double) driveGain, -0.6);   // partial level comp
        }

        inline float processSample (float x) noexcept
        {
            if (driveOn) x = std::tanh (x * driveGain) * driveMakeup;
            if (hpOn) x = hp.processSample (x);
            if (lpOn) x = lp.processSample (x);
            return x * outTrim;
        }

        void processBlock (float* data, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
                data[i] = processSample (data[i]);
        }
    };
}
