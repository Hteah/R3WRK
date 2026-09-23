#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include "BiquadFilter.h"   // reuse r3wrk::Biquad for the Tilt shelving stage

/**
    A 4-line Feedback Delay Network reverb modelled on the Make Noise Erbe-Verb (DSP design by
    Tom Erbe / SoundHack). Phase 1 only: the core network (delay lines, diffusion, damping,
    decay/saturation, Tilt EQ, pre-delay, stereo output, dry/wet mix). No Cyclic/Ergodic
    modulation, no Shimmer granular pitch-shifter, no Reverse -- each of those is a separate
    subsystem, deferred to a later pass.

    Sources (see ~/Downloads/erbe-verb-ARCHITECTURE.md for the full research notes this was
    built from):
      - Tom Erbe, "Building the Erbe-Verb: Extending the Feedback Delay Network Reverb for
        Modular Synthesizer Use" (ICMC 2015) -- qualitative structure, no equations.
      - dm-Erbeverb (https://github.com/Afturmath/dm-Erbeverb), a fan recreation in Max/MSP's
        gen~, Creative Commons Attribution 3.0 Unported. Its compiled C++ export
        (modep/src/gen_exported.cpp) is where every concrete number below (the allpass delay-
        time table, the Hadamard matrix, the saturator formula, the Tilt frequencies, the
        stereo output weights) comes from -- real, running code, not Make Noise's firmware
        (unpublished), but the closest public ground truth. Treat exact constants as a very
        well-informed starting point to tune by ear, not gospel -- see the per-constant
        comments below for which parts are paper-confirmed vs. one implementer's tuning choice.

    Deliberately not juce::dsp -- matches BiquadFilter.h/LfoModule.h's reasoning: plain
    doubles, no allocation once prepare() has sized the delay buffers, no locks, safe to tick
    from the audio thread, and it compiles into the headless smoke-test target.
*/
namespace r3wrk
{
    // A circular buffer with integer-sample addressing. No fractional/interpolated read yet --
    // nothing modulates delay time in phase 1, so plain nearest-sample is enough; Cyclic/
    // Ergodic modulation (phase 2+) will need interpolation, not added here.
    struct DelayLine
    {
        std::vector<float> buf;
        int writePos = 0;

        void prepare(double sampleRate, double maxMs)
        {
            const int n = juce::jmax(4, (int) std::ceil(sampleRate * maxMs * 0.001) + 4);
            buf.assign((size_t) n, 0.0f);
            writePos = 0;
        }
        void reset() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); writePos = 0; }

        inline float read(int delaySamples) const noexcept
        {
            const int n = (int) buf.size();
            delaySamples = juce::jlimit(0, n - 1, delaySamples);
            int idx = writePos - delaySamples;
            if (idx < 0) idx += n;
            return buf[(size_t) idx];
        }
        inline void write(float x) noexcept
        {
            buf[(size_t) writePos] = x;
            if (++writePos >= (int) buf.size()) writePos = 0;
        }
    };

    // y = coeff*y + (1-coeff)*x -- coeff near 1 tracks the input very slowly (heavy smoothing,
    // low cutoff, lots of high-frequency loss); coeff near 0 tracks it almost instantly (barely
    // any filtering). This is the convention the architecture doc's lowpass(x, coeff) calls
    // assume (dampingAmt directly as coeff, and the saturator's fixed 0.99): coeff=0.99 at a
    // typical sample rate has a time constant of ~2ms (~77Hz cutoff), matching "strip DC/low-
    // freq bias" -- the OTHER convention (coeff near 1 = fast tracking) would make that same
    // 0.99 barely filter anything, which is what an earlier version of this file had, and which
    // collapsed the saturator's output toward silence right where Decay should start sustaining
    // near-infinitely instead. Exactly coeff=1 would freeze the output at its first sampled
    // value forever, so callers keep it just short of that (see dampCoeff below).
    struct OnePoleLowpass
    {
        double y = 0.0;
        void reset() noexcept { y = 0.0; }
        inline double process(double x, double coeff) noexcept { y = coeff * y + (1.0 - coeff) * x; return y; }
    };

    // One Schroeder allpass stage over its own DelayLine -- the architecture doc's exact
    // formula, gain applied at half-strength on both taps. 16 of these total (4 per branch,
    // cascaded in series) make up the diffuser.
    struct AllpassDiffuser
    {
        DelayLine line;
        void prepare(double sampleRate, double maxMs) { line.prepare(sampleRate, maxMs); }
        void reset() noexcept { line.reset(); }

        inline float process(float input, int delaySamples, double gain) noexcept
        {
            const float readVal     = line.read(delaySamples);
            const float feedback    = readVal * (float) gain * 0.5f;
            const float input2      = input + feedback;
            const float feedforward = -input2 * (float) gain * 0.5f;
            const float output      = readVal + feedforward;
            line.write(input2);
            return output;
        }
    };

    // RBJ cookbook shelving biquads (Q form). r3wrk::Biquad (BiquadFilter.h) only exposes LP/
    // HP/BP/Notch via setCoeffs(), so Tilt's two shelves compute their own coefficients
    // directly onto the same Biquad state/processSample plumbing rather than extending that
    // shared, already-relied-upon file.
    inline void setLowShelfCoeffs(Biquad& f, double freq, double q, double gainDb, double fs) noexcept
    {
        fs = juce::jmax(1.0, fs);
        freq = juce::jlimit(10.0, fs * 0.49, freq);
        q = juce::jmax(0.05, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freq / fs;
        const double cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * q);
        const double sqrtA = std::sqrt(A);

        const double b0 =        A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha);
        const double b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
        const double b2 =        A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha);
        const double a0 =             (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
        const double a1 =       -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
        const double a2 =             (A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha;

        f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
    }

    inline void setHighShelfCoeffs(Biquad& f, double freq, double q, double gainDb, double fs) noexcept
    {
        fs = juce::jmax(1.0, fs);
        freq = juce::jlimit(10.0, fs * 0.49, freq);
        q = juce::jmax(0.05, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freq / fs;
        const double cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * q);
        const double sqrtA = std::sqrt(A);

        const double b0 =        A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha);
        const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
        const double b2 =        A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha);
        const double a0 =             (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
        const double a1 =        2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
        const double a2 =             (A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha;

        f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
    }

    // Chebyshev-perturbation saturator: T3(x) used as a SMALL (5%), highpassed perturbation on
    // top of the mostly-unchanged input, not a direct waveshaper. Using raw T3(x) directly
    // attenuates and sign-inverts small signal (T3(x) ~= -3x near zero), which kills sustain
    // exactly where a decaying tail spends most of its time -- see ErbeVerbReverb's own
    // discovery of this. `hp` is a persistent per-line/per-branch OnePoleLowpass used only for
    // this saturator's own highpass-via-lowpass-subtract step; each caller owns one per line.
    // Shared between ErbeVerbReverb and PlexiphonEngine rather than duplicated.
    inline float chebyshevPerturb(float x01, double driveAmt, OnePoleLowpass& hp) noexcept
    {
        if (driveAmt <= 0.0001)
            return x01;
        const double x        = juce::jlimit(-1.0, 1.0, (double) x01);
        const double cheby    = 4.0 * x * x * x - 3.0 * x;
        const double shaped   = (x - cheby * 0.05) * 0.877193;
        const double shapedLp = hp.process(shaped, 0.99);
        const double shapedHp = shaped - shapedLp;
        return (float) (x + driveAmt * (shapedHp - x));
    }

    // The unnormalized +/-1 Hadamard feedback matrix, scaled by decayGain (max 0.6) -- Decay
    // itself supplies the normalization the matrix doesn't. A free function (not inlined
    // straight into ErbeVerbReverb::processSample) so the offline smoke test can exercise this
    // exact matrix math directly, in isolation from the delay/diffusion network around it.
    inline void hadamardFeedback(const float fdn[4], double decayGain, double outF[4]) noexcept
    {
        outF[0] = decayGain * ( fdn[0] - fdn[1] - fdn[2] + fdn[3]);
        outF[1] = decayGain * ( fdn[0] + fdn[1] - fdn[2] - fdn[3]);
        outF[2] = decayGain * ( fdn[0] - fdn[1] + fdn[2] - fdn[3]);
        outF[3] = decayGain * ( fdn[0] + fdn[1] + fdn[2] + fdn[3]);
    }

    /**
        The 4-line FDN engine. One instance handles a full stereo pair -- the algorithm is
        inherently stereo (the matrix and the output taps are shared across the field, not run
        independently per channel). Mono callers duplicate their one sample into both stereo
        inputs; take the average of outL/outR back out for a mono channel.

        setParams() takes already-smoothed 0..1 knob positions (same division of labour as
        MultiModeFilter/ModulatedMultiModeFilter: this struct does the DSP, the caller owns
        anti-zipper smoothing via juce::SmoothedValue). Call it once per block; nothing here is
        modulated at audio rate in phase 1, so a per-block coefficient recompute is enough.
    */
    struct ErbeVerbReverb
    {
        static constexpr int kNumLines = 4;
        static constexpr int kAllpassesPerLine = 4;

        DelayLine       lines[kNumLines];
        AllpassDiffuser allpass[kNumLines][kAllpassesPerLine];
        OnePoleLowpass  damping[kNumLines];
        OnePoleLowpass  saturatorHp[kNumLines];   // highpass-via-lowpass-subtract, for the saturator
        DelayLine       predelay;
        Biquad          tiltLowL, tiltHighL, tiltLowR, tiltHighR;

        double sampleRate = 44100.0;

        // Recomputed once per setParams() call:
        double diffusionGain = 0.0, dampCoeff = 1.0, decayGain = 0.0, driveAmt = 0.0;
        int    sizeInSamplesInt = 0;
        double predelaySamples = 0.0;
        double mixWet = 0.0, mixDry = 1.0;
        double widthAmount = 1.0;   // mid-side scale on the wet signal only -- see setParams()

        // Line base delay = this fraction of the Size-scaled length ("Size" behaves like a
        // physical travel-time scale -- see setParams()). Line 4 uses the raw scaled size.
        static constexpr double kLineFrac[kNumLines] = { 0.625597, 0.719094, 0.842925, 1.0 };

        // Allpass diffuser delay times (ms), 4 cascaded per branch, all under 15ms -- matches
        // the paper's "kept under 15 milliseconds" claim. Exact ms values are the reference
        // implementation's tuning choice, not derived from the paper itself.
        static constexpr double kAllpassMs[kNumLines][kAllpassesPerLine] = {
            { 5.020833, 1.854167, 7.229167, 14.604167 },
            { 4.145833, 3.145833, 7.979167, 13.145833 },
            { 5.229167, 2.645833, 10.395833, 12.770833 },
            { 4.395833, 3.770833, 5.854167, 14.020833 }
        };

        // Early reflections: extra fixed-ratio taps into the same 4 delay lines (not separate
        // lines), scaled by the same Size parameter, per the paper's "early reflection times
        // are ... scaled with the same parameter [as Size]."
        static constexpr double kEarlyReflFrac[kNumLines] = { 0.207771, 0.357573, 0.421567, 0.501143 };
        static constexpr double kEarlyReflGain = 0.15;

        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1000.0, newSampleRate);

            // Worst case: Size knob's internal range tops out at 500 (see setParams()) --
            // sizeInSamples formula is linear in sampleRate, so converting the worst case back
            // to ms is sample-rate-independent. +5ms headroom for the early-reflection taps'
            // read/write skew.
            const double maxLineMs = 500.0 * 0.0029411765 * 1000.0 + 5.0;
            for (int l = 0; l < kNumLines; ++l)
            {
                lines[l].prepare(sampleRate, maxLineMs);
                for (int a = 0; a < kAllpassesPerLine; ++a)
                    allpass[l][a].prepare(sampleRate, 20.0);   // all table values < 15ms; headroom to 20
            }
            predelay.prepare(sampleRate, 550.0);   // pre-delay is clamped 7-500ms; headroom to 550
            reset();
        }

        void reset() noexcept
        {
            for (int l = 0; l < kNumLines; ++l)
            {
                lines[l].reset();
                damping[l].reset();
                saturatorHp[l].reset();
                for (int a = 0; a < kAllpassesPerLine; ++a)
                    allpass[l][a].reset();
            }
            predelay.reset();
            tiltLowL.reset(); tiltHighL.reset(); tiltLowR.reset(); tiltHighR.reset();
        }

        // size01/absorb01/decay01/tilt01/mix01/width01: 0..1 knob positions, already smoothed by
        // the caller. predelayMs: 7..500, already resolved by the caller from its own knob.
        void setParams(double size01, double absorb01, double decay01, double tilt01,
                        double mix01, double predelayMs, double width01 = 0.5) noexcept
        {
            size01     = juce::jlimit(0.0, 1.0, size01);
            absorb01   = juce::jlimit(0.0, 1.0, absorb01);
            decay01    = juce::jlimit(0.0, 1.0, decay01);
            tilt01     = juce::jlimit(0.0, 1.0, tilt01);
            mix01      = juce::jlimit(0.0, 1.0, mix01);
            width01    = juce::jlimit(0.0, 1.0, width01);
            predelayMs = juce::jlimit(7.0, 500.0, predelayMs);

            // Size: internal range 1..500 (the reference implementation's own clamp), scaled
            // to samples as if it's a physical travel-time calculation -- 0.0029411765 =
            // 1/340, "340" being roughly the speed of sound in m/s.
            const double sizeKnobInternal = juce::jmap(size01, 1.0, 500.0);
            sizeInSamplesInt = (int) std::round(sizeKnobInternal * sampleRate * 0.0029411765);

            // Absorb is staged: diffusion ramps to full by 30% of the knob's travel, and only
            // then does damping start climbing -- matches the manual's "10:00" landmark.
            diffusionGain = juce::jlimit(0.0, 0.8, absorb01 * 2.666667);
            const double dampingAmt = juce::jlimit(0.0, 1.0, (absorb01 - 0.3) * 1.428571);
            // dampingAmt IS the OnePoleLowpass coefficient directly now (see its comment above):
            // 0 = no damping (coeff 0, tracks instantly, bright), toward 1 = heavy high-frequency
            // loss (coeff near 1, tracks slowly, dark). Capped just short of 1.0 so max Absorb
            // still audibly rings instead of freezing the filter at its first sampled value.
            dampCoeff = juce::jmin(0.995, dampingAmt);

            // Decay: matrix gain (max 0.6 -- keeps the +/-1 Hadamard matrix's loop gain in the
            // paper's ~1.2-1.25 range) plus late-stage saturation. decayKnob% mirrors the
            // reference implementation's own 0-120 range; here it's a straight line across this
            // knob's 0..1 travel (not a replica of the hardware's own physical taper, which
            // isn't recoverable from the reference code alone) -- so saturation engages over
            // roughly the top ~17% of THIS knob's 0..1 range, not literally "1%" as the
            // original hardware's manual is read to imply.
            const double decayPercent = decay01 * 120.0;
            decayGain = juce::jlimit(0.0, 0.6, decayPercent * 0.005);
            driveAmt  = juce::jlimit(0.0, 1.0, (decayGain - 0.495) * 200.0);

            // Tilt: two shelves, exact frequencies/Q from the reference implementation, gains
            // sweeping opposite directions off one knob (matches the manual's stated ranges:
            // Low +12dB..-12dB, High -24dB..+24dB).
            const double lowGainDb  = -24.0 * tilt01 + 12.0;
            const double highGainDb =  48.0 * tilt01 - 24.0;
            setLowShelfCoeffs (tiltLowL,  300.0, 0.8, lowGainDb,  sampleRate);
            setLowShelfCoeffs (tiltLowR,  300.0, 0.8, lowGainDb,  sampleRate);
            setHighShelfCoeffs(tiltHighL, 6000.0, 0.8, highGainDb, sampleRate);
            setHighShelfCoeffs(tiltHighR, 6000.0, 0.8, highGainDb, sampleRate);

            // Equal-power (cosine) dry/wet crossfade -- smoother perceived-loudness sweep than
            // a linear one.
            const double theta = mix01 * juce::MathConstants<double>::halfPi;
            mixDry = std::cos(theta);
            mixWet = std::sin(theta);

            // Width: mid-side scale on the WET signal only, applied after the stereo output
            // taps/Tilt below, before the dry/wet mix -- the dry signal is never touched, so
            // turning this doesn't change anything when Mix is at 0. 0.5 = 1.0x (today's fixed
            // tap-weight width, unchanged), 0 = mono (side cancelled), 1.0 = 2.0x (wider).
            widthAmount = width01 * 2.0;

            predelaySamples = predelayMs * 0.001 * sampleRate;
        }

        // inL/inR: this sample's dry input pair (mono callers pass the same sample twice).
        // outL/outR: the wet+dry mixed output pair.
        inline void processSample(float inL, float inR, float& outL, float& outR) noexcept
        {
            // Pre-delay: a shared mono sum feeds branches 1 & 2 only (see below); branches 3
            // & 4 are pure feedback paths, per the reference implementation.
            const float dryMono = 0.5f * (inL + inR);
            predelay.write(dryMono);
            const float predelayed = predelay.read((int) predelaySamples);

            // Read each branch out of its delay line at its Size-scaled position, then saturate
            // on the way out (before the matrix) -- only audible near max Decay.
            float fdn[kNumLines];
            for (int l = 0; l < kNumLines; ++l)
            {
                const int delaySamples = (int) std::round(kLineFrac[l] * (double) sizeInSamplesInt);
                fdn[l] = lines[l].read(delaySamples);
                fdn[l] = chebyshevPerturb(fdn[l], driveAmt, saturatorHp[l]);
            }

            double f[4];
            hadamardFeedback(fdn, decayGain, f);

            // Damping (per-branch 1-pole LP). Input injection is uneven: only branches 1 & 2
            // get the predelayed dry signal, and branch 2 mixes with feedback3 (not feedback2)
            // -- both exactly as in the reference implementation.
            const double d1 = damping[0].process(predelayed + f[0], dampCoeff);
            const double d2 = damping[1].process(predelayed + f[2], dampCoeff);
            const double d3 = damping[2].process(f[1],              dampCoeff);
            const double d4 = damping[3].process(f[3],              dampCoeff);
            const double branchIn[kNumLines] = { d1, d2, d3, d4 };

            // Diffusion: 4 cascaded allpasses per branch, then write into the delay line for
            // next time.
            for (int l = 0; l < kNumLines; ++l)
            {
                float x = (float) branchIn[l];
                for (int a = 0; a < kAllpassesPerLine; ++a)
                {
                    const int delaySamples = (int) std::round(kAllpassMs[l][a] * 0.001 * sampleRate);
                    x = allpass[l][a].process(x, delaySamples, diffusionGain);
                }
                lines[l].write(x);
            }

            // Early reflections: extra taps into the same 4 lines, summed with a fixed gain.
            float earlyRefl = 0.0f;
            for (int l = 0; l < kNumLines; ++l)
            {
                const int tapDelay = (int) std::round(kEarlyReflFrac[l] * (double) sizeInSamplesInt);
                earlyRefl += lines[l].read(tapDelay);
            }
            earlyRefl *= (float) kEarlyReflGain;

            // Stereo output: asymmetric weighted sum across all 4 branches, crossed between L/R
            // for width, plus the shared early-reflections signal in both channels.
            float reverbL = 0.75f * fdn[0] + 0.25f * fdn[1] + 0.5f * fdn[2] + 0.5f * fdn[3] + earlyRefl;
            float reverbR = 0.25f * fdn[0] + 0.75f * fdn[1] + 0.5f * fdn[2] + 0.5f * fdn[3] + earlyRefl;

            reverbL = tiltHighL.processSample(tiltLowL.processSample(reverbL));
            reverbR = tiltHighR.processSample(tiltLowR.processSample(reverbR));

            // Width: mid-side scale on the wet pair only.
            const float wetMid  = 0.5f * (reverbL + reverbR);
            const float wetSide = 0.5f * (reverbL - reverbR) * (float) widthAmount;
            reverbL = wetMid + wetSide;
            reverbR = wetMid - wetSide;

            outL = (float) (mixDry * inL + mixWet * reverbL);
            outR = (float) (mixDry * inR + mixWet * reverbR);

            // Safety backstop, same spirit as MultiModeFilter's: a feedback network can in
            // principle still surprise under an untested parameter combination -- hard-clamp so
            // a future surprise is an ugly clip, never an ear- or speaker-endangering spike.
            outL = juce::jlimit(-4.0f, 4.0f, outL);
            outR = juce::jlimit(-4.0f, 4.0f, outR);
        }

        void processBlock(float* left, float* right, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
                processSample(left[i], right[i], left[i], right[i]);
        }
    };
}
