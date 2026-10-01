#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "ReverbEngine.h"   // reuse r3wrk::DelayLine

/**
    SHM -- a clean shimmer reverb: a smooth, clear tail with the pitch-shifted layer kept as a
    quiet bloom underneath it, never the main event. Voiced after the behaviour of a well-known
    hardware "vast" reverb (its internals are unpublished, so this matches what's documented about
    it, not its code): equal-power wet/dry balance, one bipolar Tone filter inside the loop, a
    well-behaved Freeze, and a tail that rewards modulation. Design notes: the "r3wrk -- Clean
    Shimmer Reverb Plan" doc (2026-09-29).

    Structure (per stereo sample):
      input -> 4 series allpasses per channel (diffusion)
            -> 8-line FDN: L feeds lines 0/2/4/6, R feeds 1/3/5/7; mutually prime lengths x Size;
               each line: allpass-interpolated read (Movement = a slow LFO per line) -> decay gain
               g = 10^(-3 L / (RT60 fs)) -> bipolar Tone (one-pole LP or HP) -> 8x8 Householder
            -> sign-mixed output taps (L and R decorrelated) -> Width (M/S) -> equal-power Mix.
      shimmer: wet mid -> HPF 400 Hz -> dual-head delay-line pitch shifter (60 ms Hann, +12 or
               +7 st) -> LPF 6 kHz -> gain (<= 0.25) -> tanh -> back into every line (once per
               loop, at its own gain, so octave stacks die out instead of climbing).

    Loop-gain bookkeeping: the tank alone is always stable (Householder is orthogonal, every
    g < 1), and the shimmer path's output is tanh-bounded, so nothing can run away. What CAN go
    wrong is octave stacks climbing: each pass re-injects a shifted copy that then rings for the
    tank's whole decay, so stack k+1's power relative to stack k is about shimmerGain^2 /
    (1 - g^2) (the shifted copies add in power, not amplitude -- their phases are unrelated). The
    shimmer feed is capped so that ratio stays <= 0.5 (kStackRatio below): each stack is at most
    half as strong as the one before, so they die out. The Decay gains are never touched, so RT60
    stays exact up to 20 s. (The plan doc's "max(g) + shimmer <= 0.98" rule capped Decay itself
    at ~8-12 s depending on Size -- measured -- so it was replaced with this.)

    Freeze: loop gain 1 (just under -- see kFreezeGain), input muted, shimmer muted, Tone filters
    bypassed, all crossfaded over ~30 ms so engaging it never clicks.

    Same conventions as ErbeVerbReverb: plain doubles/floats, no allocation after prepare(),
    no juce_dsp (compiles into the headless smoke test), setParams() once per block with the
    caller's already-smoothed 0..1 knob positions. Size is additionally smoothed here per sample
    (~200 ms) because it moves read positions -- block-rate steps would zipper the pitch.

    Fractional line reads use first-order allpass interpolation, not cubic/Lagrange: any FIR
    interpolator loses high end at a fractional position, and inside a loop that loss compounds
    every pass -- measured, cubic Hermite cut RT60 ~15% short and drained Freeze 2 dB in 9 s.
    The allpass is magnitude-flat, and Movement's LFOs are slow enough for its phase-only error.
*/
namespace r3wrk
{
    struct ShimmerReverb
    {
        static constexpr int kNumLines = 8;
        static constexpr int kNumDiffusers = 4;

        // Knob mappings -- public so the UI's readouts and the smoke test use the same numbers.
        static double decaySeconds(double decay01) noexcept   // 0.3 .. 20 s, exponential
        {
            return 0.3 * std::pow(20.0 / 0.3, juce::jlimit(0.0, 1.0, decay01));
        }
        static double sizeScale(double size01) noexcept { return juce::jmap(juce::jlimit(0.0, 1.0, size01), 0.3, 1.5); }
        static double shimmerGain(double shimmer01) noexcept { return 0.25 * juce::jlimit(0.0, 1.0, shimmer01); }
        // Tone: 0..1 knob, 0.5 = centre. Below: in-loop lowpass 20 kHz -> 1.5 kHz. Above: in-loop
        // highpass 20 Hz -> 600 Hz.
        static double toneLowpassHz(double tone01) noexcept
        {
            const double t = juce::jlimit(0.0, 1.0, (0.5 - tone01) * 2.0);
            return 20000.0 * std::pow(1500.0 / 20000.0, t);
        }
        static double toneHighpassHz(double tone01) noexcept
        {
            const double t = juce::jlimit(0.0, 1.0, (tone01 - 0.5) * 2.0);
            return 20.0 * std::pow(600.0 / 20.0, t);
        }

        // Base lengths at 48 kHz (scaled by fs/48k and Size).
        static constexpr double kLineBase[kNumLines] = { 1433, 1601, 1867, 2053, 2251, 2399, 2687, 2903 };
        static constexpr double kDiffBaseL[kNumDiffusers] = { 142, 107, 379, 277 };
        static constexpr double kDiffBaseR[kNumDiffusers] = { 151, 113, 389, 283 };   // slightly off L's: decorrelates
        static constexpr double kDiffGain = 0.65;
        // Movement: per-line LFO rates spread over 0.1-1.2 Hz (shuffled so neighbouring lengths
        // don't share neighbouring rates), depth 0..12 samples at 48 kHz.
        static constexpr double kLfoHz[kNumLines] = { 0.10, 0.73, 0.41, 1.20, 0.26, 0.89, 0.57, 1.04 };
        static constexpr double kMaxModSamples48k = 12.0;
        // Output tap signs: L and R use different patterns so they decorrelate.
        static constexpr float kTapL[kNumLines] = { +1, -1, +1, +1, -1, +1, -1, -1 };
        static constexpr float kTapR[kNumLines] = { +1, +1, -1, +1, +1, -1, -1, +1 };
        // Shimmer injection signs (unit norm after the 1/sqrt(8) below).
        static constexpr float kInject[kNumLines] = { +1, -1, -1, +1, -1, +1, +1, -1 };
        static constexpr double kShiftWindowMs = 60.0;
        static constexpr double kStackRatio = 0.7071;   // shimmer cap: stack power ratio <= 0.5
        static constexpr double kFreezeGain = 0.99999;   // "1", minus a hair of insurance

        struct OnePole   // y += a (x - y): lowpass; x - y: highpass
        {
            float y = 0.0f;
            void reset() noexcept { y = 0.0f; }
            inline float lp(float x, float a) noexcept { y += a * (x - y); return y; }
            inline float hp(float x, float a) noexcept { return x - lp(x, a); }
        };
        static float onePoleCoeff(double hz, double fs) noexcept
        {
            return (float) (1.0 - std::exp(-juce::MathConstants<double>::twoPi * juce::jmin(hz, 0.49 * fs) / fs));
        }

        struct Allpass
        {
            DelayLine line;
            int delay = 1;
            inline float process(float x, float g) noexcept
            {
                const float z = line.read(delay);
                const float w = x + g * z;
                line.write(w);
                return z - g * w;
            }
        };

        double fs = 48000.0;
        DelayLine lines[kNumLines];
        Allpass diffL[kNumDiffusers], diffR[kNumDiffusers];
        OnePole toneLp[kNumLines], toneHp[kNumLines];
        float apState[kNumLines] {};   // allpass interpolator's previous output, per line
        double lfoPhase[kNumLines] {};
        double lfoInc[kNumLines] {};

        // Shimmer path state.
        DelayLine shiftBuf;
        double shiftPhase = 0.0;
        OnePole shimHp1, shimHp2, shimLp1, shimLp2;
        float shimFeedback = 0.0f;   // last sample's shifted signal, injected this sample

        // Per-sample smoothed.
        double sizeCur = 0.9, sizeTarget = 0.9, sizeCoeff = 0.0;
        double freezeCur = 0.0, freezeTarget = 0.0, freezeCoeff = 0.0;

        // Per-block (setParams).
        double lineGain[kNumLines] {};
        float  aToneLp = 1.0f, aToneHp = 0.0f, toneLpAmt = 0.0f, toneHpAmt = 0.0f;
        float  aShimHp = 0.0f, aShimLp = 1.0f;
        double shimGainCur = 0.0;
        double shiftInc = 0.0, shiftWindow = 2880.0;
        double modDepth = 0.0;
        double widthAmount = 1.0;
        double mixWet = 0.0, mixDry = 1.0;
        double rt60 = 3.0;

        void prepare(double newSampleRate)
        {
            fs = juce::jmax(8000.0, newSampleRate);
            const double k = fs / 48000.0;
            const double maxLineMs = (2903.0 * 1.5 + kMaxModSamples48k + 8.0) / 48000.0 * 1000.0 + 1.0;
            for (auto& l : lines) l.prepare(fs, maxLineMs);
            for (int d = 0; d < kNumDiffusers; ++d)
            {
                diffL[d].line.prepare(fs, 10.0);
                diffR[d].line.prepare(fs, 10.0);
                diffL[d].delay = juce::jmax(1, (int) std::round(kDiffBaseL[d] * k));
                diffR[d].delay = juce::jmax(1, (int) std::round(kDiffBaseR[d] * k));
            }
            for (int l = 0; l < kNumLines; ++l)
            {
                lfoInc[l] = kLfoHz[l] / fs;
                lfoPhase[l] = (double) l / kNumLines;
            }
            shiftWindow = kShiftWindowMs * 0.001 * fs;
            shiftBuf.prepare(fs, kShiftWindowMs + 5.0);
            sizeCoeff   = 1.0 - std::exp(-1.0 / (0.2 * fs / 5.0));    // ~200 ms to settle (5 time constants)
            freezeCoeff = 1.0 - std::exp(-1.0 / (0.03 * fs / 5.0));   // ~30 ms
            aShimHp = onePoleCoeff(400.0, fs);
            aShimLp = onePoleCoeff(6000.0, fs);
            reset();
        }

        void reset() noexcept
        {
            for (auto& l : lines) l.reset();
            for (int d = 0; d < kNumDiffusers; ++d) { diffL[d].line.reset(); diffR[d].line.reset(); }
            for (int l = 0; l < kNumLines; ++l)
            {
                toneLp[l].reset(); toneHp[l].reset();
                apState[l] = 0.0f;
                lfoPhase[l] = (double) l / kNumLines;
            }
            shiftBuf.reset();
            shiftPhase = 0.0;
            shimHp1.reset(); shimHp2.reset(); shimLp1.reset(); shimLp2.reset();
            shimFeedback = 0.0f;
            sizeCur = sizeTarget;
            freezeCur = freezeTarget;
        }

        // Jump the per-sample smoothers to their targets (after reset() on an enable edge, so a
        // fresh tail doesn't glide in from stale settings).
        void snapSmoothers() noexcept { sizeCur = sizeTarget; freezeCur = freezeTarget; }

        // All 0..1 knob positions, already block-smoothed by the caller; fifth/freeze are switches.
        void setParams(double size01, double decay01, double tone01, double shimmer01, bool fifth,
                       double movement01, double width01, double mix01, bool freeze) noexcept
        {
            sizeTarget   = sizeScale(size01);
            freezeTarget = freeze ? 1.0 : 0.0;
            rt60 = decaySeconds(decay01);

            // Decay gain per line from its TARGET length (so the RT60 is right whatever order
            // setParams / snapSmoothers run in; during a ~200 ms Size glide it's briefly off by
            // the glide's ratio, inaudibly).
            const double k = fs / 48000.0;
            double gMax = 0.0;
            for (int l = 0; l < kNumLines; ++l)
            {
                const double len = kLineBase[l] * k * sizeTarget;
                lineGain[l] = std::pow(10.0, -3.0 * len / (rt60 * fs));
                gMax = juce::jmax(gMax, lineGain[l]);
            }
            shimGainCur = juce::jmin(shimmerGain(shimmer01), kStackRatio * std::sqrt(juce::jmax(0.0, 1.0 - gMax * gMax)));

            // Centre = no filter at all (even a 20 kHz one-pole costs a little every pass and
            // shortened the measured RT60 ~20%); each side fades its filter's CONTRIBUTION in over
            // the first 10% of its travel, so the knob is continuous through the centre. The
            // filters themselves always run at their real corner -- an earlier version faded the
            // highpass's coefficient to 0 instead, which froze its state, and the frozen offset
            // was re-added every pass: DC built up in the tank after Tone came back to centre.
            toneLpAmt = (float) juce::jlimit(0.0, 1.0, (0.5 - tone01) * 20.0);
            toneHpAmt = (float) juce::jlimit(0.0, 1.0, (tone01 - 0.5) * 20.0);
            aToneLp = onePoleCoeff(toneLowpassHz(tone01), fs);
            aToneHp = onePoleCoeff(toneHighpassHz(tone01), fs);

            const double ratio = fifth ? std::pow(2.0, 7.0 / 12.0) : 2.0;
            shiftInc = (ratio - 1.0) / shiftWindow;

            modDepth = juce::jlimit(0.0, 1.0, movement01) * kMaxModSamples48k * k;
            widthAmount = juce::jlimit(0.0, 1.0, width01);

            const double theta = juce::jlimit(0.0, 1.0, mix01) * juce::MathConstants<double>::halfPi;
            mixDry = std::cos(theta);
            mixWet = std::sin(theta);
        }

        inline void processSample(float inL, float inR, float& outL, float& outR) noexcept
        {
            sizeCur   += (sizeTarget - sizeCur) * sizeCoeff;
            freezeCur += (freezeTarget - freezeCur) * freezeCoeff;
            const float live = (float) (1.0 - freezeCur);   // input / shimmer / filter amount

            // Input diffusion.
            float xL = inL * live, xR = inR * live;
            for (int d = 0; d < kNumDiffusers; ++d)
            {
                xL = diffL[d].process(xL, (float) kDiffGain);
                xR = diffR[d].process(xR, (float) kDiffGain);
            }

            // Read every line (modulated, cubic), apply decay gain + Tone.
            const double k = fs / 48000.0;
            float y[kNumLines];
            for (int l = 0; l < kNumLines; ++l)
            {
                const double mod = modDepth * 0.5 * (1.0 + std::sin(juce::MathConstants<double>::twoPi * lfoPhase[l]));
                lfoPhase[l] += lfoInc[l];
                if (lfoPhase[l] >= 1.0) lfoPhase[l] -= 1.0;

                // Allpass interpolation: integer part M, fraction d kept in [0.5, 1.5) so the
                // coefficient stays well inside (-1/3, 1/3].
                const double delay = kLineBase[l] * k * sizeCur + mod;
                const int m = (int) std::floor(delay - 0.5);
                const float d = (float) (delay - (double) m);
                const float eta = (1.0f - d) / (1.0f + d);
                const float raw = eta * lines[l].read(m) + lines[l].read(m + 1) - eta * apState[l];
                apState[l] = raw;
                const double g = lineGain[l] + (kFreezeGain - lineGain[l]) * freezeCur;
                const float scaled = (float) (raw * g);
                const float lp = scaled + toneLpAmt * (toneLp[l].lp(scaled, aToneLp) - scaled);
                const float filtered = lp - toneHpAmt * toneHp[l].lp(lp, aToneHp);   // x - lowpass = highpass
                y[l] = scaled + (filtered - scaled) * live;   // Freeze bypasses Tone
            }

            // Output taps (from the pre-matrix line outputs).
            constexpr float kNorm = 0.35355339f;   // 1/sqrt(8)
            float wetL = 0.0f, wetR = 0.0f;
            for (int l = 0; l < kNumLines; ++l) { wetL += kTapL[l] * y[l]; wetR += kTapR[l] * y[l]; }
            wetL *= kNorm; wetR *= kNorm;

            // Householder: m = y - (2/N) sum(y).
            float sum = 0.0f;
            for (float v : y) sum += v;
            const float h = sum * (2.0f / kNumLines);

            // Write back: matrix output + input (L -> even indices, R -> odd) + shimmer.
            const float shim = shimFeedback * kNorm;
            for (int l = 0; l < kNumLines; ++l)
            {
                const float in = ((l & 1) == 0 ? xL : xR) * 0.5f;
                lines[l].write(y[l] - h + in + kInject[l] * shim);
            }

            // Shimmer path, from the wet mid. Its output is injected next sample.
            {
                const float tap = 0.5f * (wetL + wetR);
                const float band = shimHp2.hp(shimHp1.hp(tap, aShimHp), aShimHp);
                shiftBuf.write(band);
                const double ph2 = shiftPhase + 0.5 >= 1.0 ? shiftPhase - 0.5 : shiftPhase + 0.5;
                const float s1 = std::sin((float) (juce::MathConstants<double>::pi * shiftPhase));
                const float s2 = std::sin((float) (juce::MathConstants<double>::pi * ph2));
                const float head1 = shiftBuf.readInterpolated(2.0 + shiftWindow * (1.0 - shiftPhase));
                const float head2 = shiftBuf.readInterpolated(2.0 + shiftWindow * (1.0 - ph2));
                shiftPhase += shiftInc;
                if (shiftPhase >= 1.0) shiftPhase -= 1.0;
                const float shifted = head1 * s1 * s1 + head2 * s2 * s2;
                const float smooth = shimLp2.lp(shimLp1.lp(shifted, aShimLp), aShimLp);
                shimFeedback = std::tanh(smooth * (float) shimGainCur) * live;
            }

            // Width (M/S on the wet only), then equal-power Mix.
            const float mid = 0.5f * (wetL + wetR);
            const float side = 0.5f * (wetL - wetR) * (float) widthAmount;
            outL = (float) (mixDry * inL + mixWet * (mid + side));
            outR = (float) (mixDry * inR + mixWet * (mid - side));

            // Same safety backstop as ErbeVerbReverb.
            outL = juce::jlimit(-4.0f, 4.0f, outL);
            outR = juce::jlimit(-4.0f, 4.0f, outR);
        }
    };
}
