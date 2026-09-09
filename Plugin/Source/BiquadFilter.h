#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "MonomachineFilterModel.h"   // generated: r3wrk::mnm::{hpCutoffHz,lpCutoffHz,resonanceToQ,engaged}

/**
    A tiny hand-rolled RBJ biquad (transposed direct-form II), plus r3wrk::MultiModeFilter --
    the Elektron Monomachine multimode filter (Base / Width / HP Q / LP Q) built from two of
    them, using the empirical model in MonomachineFilterModel.h. Deliberately NOT juce::dsp:
    AudioDocument.cpp bakes the same filter into Save/Export and is compiled into the headless
    smoke-test target, which doesn't link juce_dsp. So the exact same math is shared by the
    real-time playback path (PluginProcessor) and the offline bake
    (AudioDocument::renderWithPlaybackKnobs).

    Biquad modes: 0 = off, 1 = low-pass, 2 = high-pass, 3 = band-pass (constant-skirt,
    ~0 dB peak), 4 = notch. 12 dB/oct.
*/
namespace r3wrk
{
    enum FilterMode { filterOff = 0, filterLP = 1, filterHP = 2, filterBP = 3, filterNotch = 4 };

    // Frequency (20 Hz .. 20 kHz) -> a 0..1 edge position on a plain log scale. Only used to
    // migrate the oldest (pre-Base/Width) saved sessions' mode/cutoff filter onto a Base or
    // Width knob value; the live filter uses the Monomachine model (MonomachineFilterModel.h).
    inline double filterHzToPos (double hz)
    {
        return juce::jlimit (0.0, 1.0, std::log (juce::jmax (20.0, hz) / 20.0) / std::log (1000.0));
    }

    // The two edge frequencies for a Base/Width pair, from the Monomachine model: Base sets
    // the high-pass corner, Base+Width the low-pass corner. Used for the knob readouts.
    inline void filterEdges (double base01, double width01, double& fLowEdge, double& fHighEdge)
    {
        fLowEdge  = mnm::hpCutoffHz (base01);
        fHighEdge = mnm::lpCutoffHz (base01, width01);
    }

    // "Engaged" == the filter would change the sound (Base ~0 => no high-pass; Base+Width ~1
    // => no low-pass; either Q up => a resonant bump even with the edge open). Otherwise
    // callers skip it and don't count it as a playback knob that forces the slow RubberBand /
    // offline render path.
    inline bool filterEngaged (double base01, double width01, double hpQ01 = 0.0, double lpQ01 = 0.0)
    {
        return mnm::engaged (base01, width01, hpQ01, lpQ01);
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
        The Elektron Monomachine multimode filter, modelled from measurements (see
        MonomachineFilterModel.h and ~/Documents/Claude/MNMFILTER). A 2-pole high-pass at the
        Base corner and a 2-pole low-pass at the Base+Width corner, in series, with independent
        resonance (HP Q / LP Q) on each. You dial the two edges of the passband directly:

            Base 0    + Width 1    -> both edges open, wide open (no effect)
            Base 0    + Width mid  -> low-pass
            Base mid  + Width 1    -> high-pass
            Base mid  + Width small-> narrow, resonant band

        Corner frequencies come from the fitted curves (HP fc doubles about every 8 Base units;
        the LP corner sits ~an octave below the HP one at equal param). Each edge stage
        self-bypasses when it isn't doing anything, so an "open" filter is a true passthrough.

        HP Q / LP Q (0..1) map to biquad Q via the measured resonance curve: near unity Q the
        Monomachine's filter self-oscillates and the resonant peak measured 25-30 dB. That's
        faithful but can get very loud, so a mild Q-dependent output trim pulls narrow,
        heavily-resonant settings back toward sanity (a deliberate deviation from the hardware).
    */
    struct MultiModeFilter
    {
        Biquad hp, lp;
        bool  hpOn = false, lpOn = false;
        float outTrim = 1.0f;

        void reset() noexcept { hp.reset(); lp.reset(); }

        // base01 / width01 / hpQ01 / lpQ01 are the four knob values, 0..1.
        void setParams (double base01, double width01, double hpQ01, double lpQ01, double fs) noexcept
        {
            base01  = juce::jlimit (0.0, 1.0, base01);
            width01 = juce::jlimit (0.0, 1.0, width01);
            hpQ01   = juce::jlimit (0.0, 1.0, hpQ01);
            lpQ01   = juce::jlimit (0.0, 1.0, lpQ01);

            const double fLow  = mnm::hpCutoffHz (base01);
            const double fHigh = mnm::lpCutoffHz (base01, width01);
            const double nyq   = juce::jmax (1.0, fs) * 0.49;

            hpOn = fLow > 20.0 || hpQ01 > 0.02;
            lpOn = fHigh < nyq || lpQ01 > 0.02;

            if (hpOn)
                hp.setCoeffs (filterHP, juce::jmax (20.0, fLow), mnm::resonanceToQ (hpQ01), fs);
            if (lpOn)
                lp.setCoeffs (filterLP, juce::jlimit (juce::jmax (30.0, fLow * 1.02), nyq, fHigh),
                              mnm::resonanceToQ (lpQ01), fs);

            // A narrow, heavily-resonant band can stack both peaks and scream. Trim back
            // toward unity as the band narrows with Q up; unity by ~half Width or low Q.
            const double gap = juce::jlimit (0.0, 1.0, base01 + width01) - base01;
            const double qMax = juce::jmax (hpQ01, lpQ01);
            outTrim = (hpOn && lpOn)
                ? (float) (1.0 / (1.0 + 6.0 * qMax * juce::jmax (0.0, 0.35 - gap)))
                : (float) (1.0 / (1.0 + 1.5 * juce::jmax (0.0, qMax - 0.6)));
        }

        inline float processSample (float x) noexcept
        {
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
