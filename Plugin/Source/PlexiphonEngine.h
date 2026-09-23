#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <array>
#include "ReverbEngine.h"   // reuse DelayLine, AllpassDiffuser, OnePoleLowpass, Biquad, shelf
                            // helpers, and chebyshevPerturb -- both engines are FDN-family
                            // reverbs built the same way.

/**
    An 8-line Feedback Delay Network whose core trick -- per the Make Noise/Tom Erbe Plexiphon,
    a 2026 module with no published paper or reverse-engineered reference implementation yet
    (unlike the Erbe-Verb) -- is continuously morphing the feedback matrix's own topology: from
    a sparse permutation (independent, non-interacting delay taps -- "multi-tap echo") to a
    dense orthonormal Hadamard (densely interconnected paths -- "reverb"), with no mode-
    switching. See ~/Downloads/PLEXIPHON_PLAN.md for the full research notes and reasoning this
    was built from.

    This is thinner ground than ReverbEngine.h: no delay-time table or matrix coefficients to
    quote from a hardware source, since none exist publicly yet. The matrix-interpolation
    scheme below is this project's own design, grounded in the time-varying-feedback-matrix
    reverb-DSP literature (Schlecht & Habets, cited in PLEXIPHON_PLAN.md) rather than invented
    from nothing, and verified offline (Tests/SmokeTest.cpp, "Plexus matrix interpolation
    stability") before being trusted in the full engine -- the same discipline that caught the
    Erbe-Verb saturator bug.
*/
namespace r3wrk
{
    // The interpolated N x N feedback matrix at the heart of Plexus. Deliberately its own
    // struct (not inlined into PlexiphonEngine) so the offline smoke test can exercise this
    // exact matrix math in isolation -- the same reasoning ErbeVerbReverb's hadamardFeedback()
    // free function was split out for.
    struct PlexusMatrix
    {
        static constexpr int N = 8;
        using Row = std::array<double, N>;

        // Sparse endpoint: an 8-cycle permutation (each line's feedback goes to exactly one
        // OTHER line, forming one loop) -- exactly orthonormal by construction (a permutation
        // matrix has unit row/column norms and eigenvalues on the unit circle), so it's an
        // unconditionally stable "independent taps" endpoint with no tuning needed.
        static std::array<Row, N> sparseMatrix() noexcept
        {
            std::array<Row, N> m{};
            for (int i = 0; i < N; ++i)
            {
                for (int j = 0; j < N; ++j)
                    m[(size_t) i][(size_t) j] = 0.0;
                m[(size_t) i][(size_t) ((i + 1) % N)] = 1.0;
            }
            return m;
        }

        // Dense endpoint: an 8x8 Hadamard matrix built by Sylvester doubling of the same 4x4
        // Hadamard ErbeVerbReverb::hadamardFeedback() uses (H8 = [[H4,H4],[H4,-H4]]),
        // normalized by 1/sqrt(8) so every row has unit L2 norm. Unlike Erbe-Verb's
        // deliberately-unnormalized Hadamard (which relies on Decay's own gain cap for safety
        // instead), Plexus needs both endpoints properly orthonormal for setAmount()'s
        // interpolation to mean anything.
        static std::array<Row, N> denseMatrix() noexcept
        {
            constexpr double h4[4][4] = {
                {  1.0, -1.0, -1.0,  1.0 },
                {  1.0,  1.0, -1.0, -1.0 },
                {  1.0, -1.0,  1.0, -1.0 },
                {  1.0,  1.0,  1.0,  1.0 },
            };
            std::array<Row, N> m{};
            const double scale = 1.0 / std::sqrt((double) N);
            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    m[(size_t) i][(size_t) j]             =  h4[i][j] * scale;
                    m[(size_t) i][(size_t) (j + 4)]       =  h4[i][j] * scale;
                    m[(size_t) (i + 4)][(size_t) j]       =  h4[i][j] * scale;
                    m[(size_t) (i + 4)][(size_t) (j + 4)] = -h4[i][j] * scale;
                }
            }
            return m;
        }

        std::array<Row, N> rows = sparseMatrix();

        // Blends toward the dense endpoint by `amount` (0 = fully sparse/permutation, 1 = fully
        // dense/Hadamard), then row-renormalizes each row back to unit L2 norm. This is a
        // pragmatic middle ground, not the fully rigorous geodesic/matrix-logarithm
        // interpolation the reverb-DSP literature describes for exactly this problem (that
        // needs real matrix-exponential machinery, disproportionate here with no numerics
        // library) -- the actual energy behavior across the sweep is settled empirically by the
        // offline stability test (SmokeTest.cpp), not asserted by this comment. Row-
        // renormalization keeps each line's own feedback gain bounded even where the blend
        // isn't perfectly orthogonal -- the same pragmatic-but-tested safety spirit as the rest
        // of this project's DSP (paired with Decay's own gain cap and a hard output clamp in
        // PlexiphonEngine, same as ErbeVerbReverb's safety backstop).
        void setAmount(double amount) noexcept
        {
            amount = juce::jlimit(0.0, 1.0, amount);
            const auto sparse = sparseMatrix();
            const auto dense  = denseMatrix();

            for (int i = 0; i < N; ++i)
            {
                double normSq = 0.0;
                for (int j = 0; j < N; ++j)
                {
                    const double v = (1.0 - amount) * sparse[(size_t) i][(size_t) j]
                                    + amount * dense[(size_t) i][(size_t) j];
                    rows[(size_t) i][(size_t) j] = v;
                    normSq += v * v;
                }
                const double norm = std::sqrt(juce::jmax(1.0e-9, normSq));
                for (int j = 0; j < N; ++j)
                    rows[(size_t) i][(size_t) j] /= norm;
            }

            // Row-renormalization alone does NOT control the whole matrix's dominant
            // eigenvalue -- measured offline (Tests/SmokeTest.cpp), a linear blend of these two
            // orthonormal endpoints can have per-bounce energy growth up to ~7-8% at
            // intermediate Plexus settings even after row-renormalization, which compounds to
            // an enormous (>1e30x) energy multiplier over the hundreds of feedback bounces a
            // real decay tail involves -- exactly the risk PLEXIPHON_PLAN.md flagged. Estimate
            // the row-normalized matrix's own spectral radius via power iteration (a handful of
            // matrix-vector multiplies; cheap, and only run once per setAmount() call, not per
            // sample) and divide the whole matrix by it, so repeatedly applying this matrix
            // alone neither grows nor shrinks energy at ANY Plexus setting -- giving
            // PlexiphonEngine's Decay knob a uniform, predictable meaning across the whole
            // Plexus range instead of one that varies with where Plexus happens to be set.
            const double radius = estimateSpectralRadius();
            if (radius > 1.0e-6)
                for (auto& row : rows)
                    for (auto& v : row)
                        v /= radius;
        }

        // out = rows * in (matrix-vector multiply). `in` and `out` must not alias.
        void apply(const double* in, double* out) const noexcept
        {
            for (int i = 0; i < N; ++i)
            {
                double acc = 0.0;
                for (int j = 0; j < N; ++j)
                    acc += rows[(size_t) i][(size_t) j] * in[j];
                out[i] = acc;
            }
        }
        void apply(const float* in, float* out) const noexcept
        {
            for (int i = 0; i < N; ++i)
            {
                double acc = 0.0;
                for (int j = 0; j < N; ++j)
                    acc += rows[(size_t) i][(size_t) j] * (double) in[j];
                out[i] = (float) acc;
            }
        }

        // Power iteration: repeatedly apply the matrix to a unit vector and renormalize; the
        // vector's own growth rate converges to the matrix's dominant eigenvalue magnitude
        // (spectral radius). A plain "last step only" estimate (tried first, empirically)
        // converges poorly here -- this matrix mixes a pure-rotation permutation with a
        // Hadamard, which can produce eigenvalues of nearly-tied magnitude near the top, the
        // classic case where power iteration oscillates step-to-step instead of settling.
        // Fixed by running a long warmup (lets the starting vector's transient components die
        // out) followed by a geometric-mean measurement over many further steps (averages out
        // any remaining oscillation from near-tied eigenvalues) -- still a practical estimate,
        // not a rigorous eigenvalue solver, validated empirically by the offline stability test
        // rather than proven mathematically, same spirit as the rest of this interpolation
        // scheme. Still cheap: ~400 8-dimensional matrix-vector multiplies, once per
        // setAmount() call (block-rate), nowhere near per-sample cost.
        double estimateSpectralRadius() const noexcept
        {
            double v[N];
            for (auto& x : v) x = 1.0 / std::sqrt((double) N);

            constexpr int kWarmup  = 200;
            constexpr int kMeasure = 200;
            double logGrowthSum = 0.0;

            for (int it = 0; it < kWarmup + kMeasure; ++it)
            {
                double out[N];
                apply(v, out);
                double normSq = 0.0;
                for (double x : out) normSq += x * x;
                const double norm = std::sqrt(juce::jmax(1.0e-15, normSq));   // v was unit-norm
                if (it >= kWarmup)
                    logGrowthSum += std::log(juce::jmax(1.0e-15, norm));
                for (int i = 0; i < N; ++i)
                    v[i] = out[i] / norm;
            }
            return std::exp(logGrowthSum / (double) kMeasure);
        }
    };

    /**
        The Plexiphon's core network: 8 delay lines recirculating through a PlexusMatrix whose
        blend amount is driven by the Plexus knob. One instance handles a full stereo pair, same
        reasoning as ErbeVerbReverb: the network is shared across the field in this phase (Couple
        -- two independent, cross-fed L/R networks -- is deferred to a later pass).

        Reuses r3wrk::DelayLine, AllpassDiffuser, OnePoleLowpass, Biquad (+ shelf helpers) and
        chebyshevPerturb from ReverbEngine.h. setParams() takes already-smoothed 0..1 knob
        positions, same division of labour as ErbeVerbReverb (the caller owns anti-zipper
        smoothing via juce::SmoothedValue).
    */
    struct PlexiphonEngine
    {
        static constexpr int kNumLines = PlexusMatrix::N;
        static constexpr int kAllpassesPerLine = 3;   // PLEXIPHON_PLAN.md suggests 2-4

        DelayLine       lines[kNumLines];
        AllpassDiffuser allpass[kNumLines][kAllpassesPerLine];
        OnePoleLowpass  damping[kNumLines];        // Diffuse's own mild static filtering component
        Biquad          colorFilter[kNumLines];    // Color's slowly-evolving shelf, see setParams()
        OnePoleLowpass  saturatorHp[kNumLines];     // for chebyshevPerturb's highpass-via-subtract
        PlexusMatrix    matrix;

        double sampleRate = 44100.0;

        double diffusionGain = 0.0, dampCoeff = 0.0;
        double decayGain = 0.0, driveAmt = 0.0;
        int    sizeInSamplesInt = 0;
        double mixWet = 0.0, mixDry = 1.0;
        double levelGain = 1.0;

        // Color: an always-on, internal slow evolution -- see this struct's header comment and
        // PLEXIPHON_PLAN.md's Color section ("spectral trajectory... brighter/darker over
        // time", not a static tone control). Not user-routable via the AudioDocument/LFO-panel
        // modulation system -- that system targets specific existing knobs by design; this is
        // an intrinsic character of the effect itself, always running whenever it's engaged.
        double colorPhase = 0.0;
        double colorRateHz = 0.1;   // sub-1Hz per the plan; own design choice, tune by ear
        double colorBias = 0.0, colorDepth = 0.0;

        // No ground-truth delay-time table exists for this module (unlike Erbe-Verb's, sourced
        // from a real reference implementation) -- these are this project's own deliberately
        // irregular spread (avoiding simple integer ratios between lines, which read as
        // periodic/metallic), not measurements. Line fractions of the Size-scaled length:
        static constexpr double kLineFrac[kNumLines] =
            { 0.615, 0.774, 0.693, 0.958, 0.842, 1.0, 0.881, 0.727 };
        // Allpass diffuser delay times (ms), 3 cascaded per line, kept under ~15ms per branch --
        // the same "short allpasses" convention Erbe-Verb's own (sourced) reference used, though
        // unconfirmed for this module specifically:
        static constexpr double kAllpassMs[kNumLines][kAllpassesPerLine] = {
            { 2.3, 5.1, 9.8 }, { 3.7, 6.9, 11.2 }, { 2.9, 7.4, 10.1 }, { 4.2, 8.3, 12.6 },
            { 3.1, 6.2, 9.4 }, { 4.8, 7.7, 13.1 }, { 2.6, 8.9, 11.8 }, { 3.9, 6.5, 10.9 },
        };

        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1000.0, newSampleRate);
            const double maxLineMs = 500.0 * 0.0029411765 * 1000.0 + 5.0;   // same Size scale as Erbe-Verb
            for (int l = 0; l < kNumLines; ++l)
            {
                lines[l].prepare(sampleRate, maxLineMs);
                for (int a = 0; a < kAllpassesPerLine; ++a)
                    allpass[l][a].prepare(sampleRate, 20.0);
            }
            reset();
        }

        void reset() noexcept
        {
            for (int l = 0; l < kNumLines; ++l)
            {
                lines[l].reset();
                damping[l].reset();
                saturatorHp[l].reset();
                colorFilter[l].reset();
                for (int a = 0; a < kAllpassesPerLine; ++a)
                    allpass[l][a].reset();
            }
        }

        // level01/plexus01/size01/diffuse01/decay01/color01/mix01: 0..1 knob positions, already
        // smoothed by the caller. blockNumSamples: this call's block length, needed to advance
        // Color's internal phase by real elapsed time (it evolves continuously, not per-knob-turn).
        void setParams(double level01, double plexus01, double size01, double diffuse01,
                       double decay01, double color01, double mix01, int blockNumSamples) noexcept
        {
            level01   = juce::jlimit(0.0, 1.0, level01);
            plexus01  = juce::jlimit(0.0, 1.0, plexus01);
            size01    = juce::jlimit(0.0, 1.0, size01);
            diffuse01 = juce::jlimit(0.0, 1.0, diffuse01);
            decay01   = juce::jlimit(0.0, 1.0, decay01);
            color01   = juce::jlimit(0.0, 1.0, color01);
            mix01     = juce::jlimit(0.0, 1.0, mix01);

            levelGain = juce::Decibels::decibelsToGain(juce::jmap(level01, 0.0, 1.0, -24.0, 12.0));

            // Plexus: the central control -- continuously morphs the feedback matrix's own
            // topology (see PlexusMatrix::setAmount()), not a gain or filter sweep.
            matrix.setAmount(plexus01);

            // Size: identical formula to ErbeVerbReverb's (sampleRate/340 physical-travel-time
            // scale) -- per PLEXIPHON_PLAN.md, no separate mechanism is needed, the delay-time-
            // vs-room-size dual character falls out of Plexus's own topology, not a special case.
            const double sizeKnobInternal = juce::jmap(size01, 1.0, 500.0);
            sizeInSamplesInt = (int) std::round(sizeKnobInternal * sampleRate * 0.0029411765);

            // Diffuse: "a combination filtering and early reflection control" per the manual --
            // the allpass cascade is the main effect; dampCoeff is a mild secondary static
            // filtering component (Color, below, is the separate SLOWLY-EVOLVING filter).
            diffusionGain = juce::jlimit(0.0, 0.75, diffuse01 * 0.75);
            dampCoeff     = juce::jlimit(0.0, 0.3, diffuse01 * 0.3);

            // Decay: "can be set to super-infinite at max" per the manual -- pushed further past
            // unity than Erbe-Verb's own range, leaning on the same saturator (chebyshevPerturb,
            // ReverbEngine.h) for stability at the top. Because PlexusMatrix::setAmount() already
            // normalizes the matrix's own spectral radius to ~1 (verified offline, SmokeTest.cpp)
            // across the whole Plexus range, decayGain here more directly *is* the loop gain --
            // unlike Erbe-Verb's cap, which was compensating for a deliberately-unnormalized
            // matrix. Starting cap 0.92: real margin under instability, but high enough for
            // genuinely long sustain; the exact ceiling is a by-ear tuning call, same as Erbe-
            // Verb's saturator was.
            decayGain = juce::jlimit(0.0, 0.92, decay01 * 0.92);
            driveAmt  = juce::jlimit(0.0, 1.0, (decayGain - 0.75) * 6.0);

            // Color: the one genuinely new design piece here (not a reconstruction) -- an
            // always-on internal evolution, not a static per-pass tone control. colorBias sets a
            // static brightness (CCW darker, CW brighter); colorDepth grows with distance from
            // centre, so the evolution itself is more pronounced the further Color is turned
            // either way and quiescent near centre. Expect to revise by ear against the
            // reference demo audio linked in PLEXIPHON_PLAN.md.
            const double colorSigned = (color01 - 0.5) * 2.0;   // -1..+1
            colorBias  = colorSigned;
            colorDepth = std::abs(colorSigned);
            colorPhase += colorRateHz * ((double) juce::jmax(0, blockNumSamples) / sampleRate)
                         * juce::MathConstants<double>::twoPi;
            if (colorPhase > juce::MathConstants<double>::twoPi)
                colorPhase = std::fmod(colorPhase, juce::MathConstants<double>::twoPi);

            const double colorGainDb = juce::jlimit(-12.0, 12.0,
                9.0 * colorBias + 4.0 * colorDepth * std::sin(colorPhase));
            for (int l = 0; l < kNumLines; ++l)
                setLowShelfCoeffs(colorFilter[l], 900.0, 0.7, colorGainDb, sampleRate);

            // Mix: equal-power (cosine) dry/wet crossfade, same as Erbe-Verb.
            const double theta = mix01 * juce::MathConstants<double>::halfPi;
            mixDry = std::cos(theta);
            mixWet = std::sin(theta);
        }

        inline void processSample(float inL, float inR, float& outL, float& outR) noexcept
        {
            const float dryMono = 0.5f * (inL + inR) * (float) levelGain;

            // Read each line out, apply Color's evolving shelf, then Decay's saturator -- all
            // before the matrix mix, mirroring ErbeVerbReverb's "saturate on the way out" order.
            float fdn[kNumLines];
            for (int l = 0; l < kNumLines; ++l)
            {
                const int delaySamples = (int) std::round(kLineFrac[l] * (double) sizeInSamplesInt);
                fdn[l] = lines[l].read(delaySamples);
                fdn[l] = colorFilter[l].processSample(fdn[l]);
                fdn[l] = chebyshevPerturb(fdn[l], driveAmt, saturatorHp[l]);
            }

            // Plexus matrix: the morphing feedback topology itself.
            float matrixIn[kNumLines], matrixOut[kNumLines];
            for (int l = 0; l < kNumLines; ++l)
                matrixIn[l] = fdn[l] * (float) decayGain;
            matrix.apply(matrixIn, matrixOut);

            // Input injection: evenly into every line (no hardware injection topology to
            // reference, unlike Erbe-Verb's sourced 2-branch pattern -- a symmetric N-line
            // network with even injection is the more natural default here).
            for (int l = 0; l < kNumLines; ++l)
            {
                double branchIn = (double) matrixOut[l] + (double) dryMono;
                branchIn = damping[l].process(branchIn, dampCoeff);

                float x = (float) branchIn;
                for (int a = 0; a < kAllpassesPerLine; ++a)
                {
                    const int delaySamples = (int) std::round(kAllpassMs[l][a] * 0.001 * sampleRate);
                    x = allpass[l][a].process(x, delaySamples, diffusionGain);
                }
                lines[l].write(x);
            }

            // Stereo output: even lines weighted toward L, odd toward R, with cross-bleed --
            // own design (no stereo-tap pattern to reference), the same "asymmetric weighted
            // sum, not a hard split" spirit as Erbe-Verb's 4-line taps.
            float sumL = 0.0f, sumR = 0.0f;
            for (int l = 0; l < kNumLines; ++l)
            {
                const bool evenLine = (l % 2) == 0;
                sumL += fdn[l] * (evenLine ? 1.0f : 0.5f);
                sumR += fdn[l] * (evenLine ? 0.5f : 1.0f);
            }
            const float wetL = sumL * 0.18f;
            const float wetR = sumR * 0.18f;

            outL = (float) (mixDry * inL + mixWet * wetL);
            outR = (float) (mixDry * inR + mixWet * wetR);

            // Safety backstop, same spirit as ErbeVerbReverb's.
            outL = juce::jlimit(-4.0f, 4.0f, outL);
            outR = juce::jlimit(-4.0f, 4.0f, outR);
        }
    };
}
