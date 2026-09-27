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

        // Sparse (echo) endpoint: the identity -- each line feeds only ITSELF, so every active
        // line is an independent echo loop repeating at its own delay time. (v1 used an 8-cycle
        // permutation, line i -> line i+1: with only one or two lines fed and heard, a sound
        // then had to travel round all 8 lines before coming back, ~4.4x Size between echoes
        // instead of the rhythmic ratios.) Exactly orthonormal, so the blend / renormalise /
        // spectral-radius scheme below applies unchanged.
        static std::array<Row, N> sparseMatrix() noexcept
        {
            std::array<Row, N> m{};
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j)
                    m[(size_t) i][(size_t) j] = (i == j) ? 1.0 : 0.0;
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
        Plexiphon v2: two 8-line feedback delay networks (L and R), each a PlexusMatrix-mixed FDN,
        built to sound unlike the Erbe-Verb (see ~/Downloads/plexiphon-differentiation.md for the
        diagnosis). PLEXUS now morphs four things together:
          - how many lines are active (fed and heard): 1 at 0%, all 8 at 100% -- w[l];
          - their lengths: rhythmic echo ratios at 0%, the irregular reverb spread at 100%;
          - the feedback wiring: each line feeds only itself at 0%, a dense Hadamard at 100%;
          - diffusion: the allpass smear only comes in from ~15% up; below that DIFFUSE softens
            each repeat with the in-loop low-pass instead, so echoes stay discrete.
        COUPLE rotates each L line into its R partner inside the feedback (energy-preserving);
        SKEW pushes L and R's PLEXUS / SIZE / COLOR apart in opposite directions. COLOR is a
        cut-only tilt inside the loop (never boosts, so it can't lift loop gain past DECAY).
        Delay lengths glide (80 ms slew, interpolated reads), so SIZE/PLEXUS moves bend pitch
        like tape instead of clicking.

        Reuses DelayLine, AllpassDiffuser, OnePoleLowpass, Biquad (+ shelf helpers) and
        chebyshevPerturb from ReverbEngine.h, unchanged. setParams() takes already-smoothed
        0..1 knob positions (the caller owns anti-zipper smoothing), called once per block.
    */
    struct PlexiphonEngine
    {
        static constexpr int kNumLines = PlexusMatrix::N;
        static constexpr int kAllpassesPerLine = 3;

        // Reverb end: the v1 irregular spread (no simple ratios between lines -> not metallic).
        static constexpr double kLineFrac[kNumLines] =
            { 0.615, 0.774, 0.693, 0.958, 0.842, 1.0, 0.881, 0.727 };
        // Echo end: rhythmic ratios, ordered so the first lines to fade in are the most musical
        // (whole, half, dotted half, third, two-thirds, quarter, 7/8, eighth).
        static constexpr double kLineFracEcho[kNumLines] =
            { 1.0, 0.5, 0.75, 1.0 / 3.0, 2.0 / 3.0, 0.25, 0.875, 0.125 };
        static constexpr double kAllpassMs[kNumLines][kAllpassesPerLine] = {
            { 2.3, 5.1, 9.8 }, { 3.7, 6.9, 11.2 }, { 2.9, 7.4, 10.1 }, { 4.2, 8.3, 12.6 },
            { 3.1, 6.2, 9.4 }, { 4.8, 7.7, 13.1 }, { 2.6, 8.9, 11.8 }, { 3.9, 6.5, 10.9 },
        };
        static constexpr double kOutGain = 0.4;       // wet level per sqrt(active lines)
        static constexpr double kColorShelfHz = 800.0;
        static constexpr double kColorShelfQ  = 0.5;  // < 0.707: these shelves don't overshoot
                                                      // 0 dB, so a cut never boosts anywhere

        struct Side
        {
            DelayLine       lines[kNumLines];
            AllpassDiffuser allpass[kNumLines][kAllpassesPerLine];
            OnePoleLowpass  damping[kNumLines];
            Biquad          colorLo[kNumLines], colorHi[kNumLines];
            OnePoleLowpass  saturatorHp[kNumLines];
            PlexusMatrix    matrix;
            double lastPlexus = -1.0;
            double w[kNumLines] {};                 // line activity (input injection + output taps)
            double targetDelay[kNumLines] {}, curDelay[kNumLines] {};
            double diffusionGain = 0.0, dampCoeff = 0.0, outGain = kOutGain;
            double colorPhase = 0.0, colorRateHz = 0.1;
        };
        Side side[2];

        double sampleRate = 44100.0;
        double decayGain = 0.0, driveAmt = 0.0;
        double levelGain = 1.0, mixWet = 0.0, mixDry = 1.0;
        double coupleCos = 1.0, coupleSin = 0.0;
        double delaySlew = 0.0;
        int    allpassDelay[kNumLines][kAllpassesPerLine] {};   // samples, fixed per sample rate
        bool   snapDelays = true;

        static double smoothstep(double e0, double e1, double x) noexcept
        {
            const double t = juce::jlimit(0.0, 1.0, (x - e0) / (e1 - e0));
            return t * t * (3.0 - 2.0 * t);
        }

        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1000.0, newSampleRate);
            const double maxLineMs = 500.0 * 0.0029411765 * 1000.0 + 5.0;
            for (auto& sd : side)
                for (int l = 0; l < kNumLines; ++l)
                {
                    sd.lines[l].prepare(sampleRate, maxLineMs);
                    for (int a = 0; a < kAllpassesPerLine; ++a)
                        sd.allpass[l][a].prepare(sampleRate, 20.0);
                }
            delaySlew = 1.0 - std::exp(-1.0 / (0.08 * sampleRate));   // ~80 ms glide
            for (int l = 0; l < kNumLines; ++l)
                for (int a = 0; a < kAllpassesPerLine; ++a)
                    allpassDelay[l][a] = (int) std::round(kAllpassMs[l][a] * 0.001 * sampleRate);
            side[0].colorRateHz = 0.10; side[0].colorPhase = 0.0;
            side[1].colorRateHz = 0.13; side[1].colorPhase = juce::MathConstants<double>::halfPi;
            reset();
        }

        void reset() noexcept
        {
            for (auto& sd : side)
            {
                for (int l = 0; l < kNumLines; ++l)
                {
                    sd.lines[l].reset();
                    sd.damping[l].reset();
                    sd.saturatorHp[l].reset();
                    sd.colorLo[l].reset();
                    sd.colorHi[l].reset();
                    for (int a = 0; a < kAllpassesPerLine; ++a)
                        sd.allpass[l][a].reset();
                }
                sd.lastPlexus = -1.0;
            }
            snapDelays = true;
        }

        // level/plexus/size/diffuse/decay/color/mix/couple/skew: 0..1 knob positions, already
        // smoothed by the caller. blockNumSamples advances COLOR's slow internal motion.
        void setParams(double level01, double plexus01, double size01, double diffuse01,
                       double decay01, double color01, double mix01, int blockNumSamples,
                       double couple01 = 0.5, double skew01 = 0.5) noexcept
        {
            level01  = juce::jlimit(0.0, 1.0, level01);
            decay01  = juce::jlimit(0.0, 1.0, decay01);
            mix01    = juce::jlimit(0.0, 1.0, mix01);
            diffuse01 = juce::jlimit(0.0, 1.0, diffuse01);
            couple01 = juce::jlimit(0.0, 1.0, couple01);
            const double skew = (juce::jlimit(0.0, 1.0, skew01) - 0.5) * 2.0;   // -1..+1

            levelGain = juce::Decibels::decibelsToGain(juce::jmap(level01, 0.0, 1.0, -24.0, 12.0));
            decayGain = juce::jlimit(0.0, 0.92, decay01 * 0.92);
            driveAmt  = juce::jlimit(0.0, 1.0, (decayGain - 0.75) * 6.0);

            const double theta = couple01 * juce::MathConstants<double>::pi * 0.25;   // 0 .. 45 deg
            coupleCos = std::cos(theta);
            coupleSin = std::sin(theta);

            const double mt = mix01 * juce::MathConstants<double>::halfPi;
            mixDry = std::cos(mt);
            mixWet = std::sin(mt);

            for (int s = 0; s < 2; ++s)
            {
                auto& sd = side[s];
                const double sign = s == 0 ? 1.0 : -1.0;   // SKEW: L one way, R the other
                const double p  = juce::jlimit(0.0, 1.0, juce::jlimit(0.0, 1.0, plexus01) + sign * 0.25 * skew);
                const double sz = juce::jlimit(0.0, 1.0, juce::jlimit(0.0, 1.0, size01)  + sign * 0.15 * skew);
                const double co = juce::jlimit(0.0, 1.0, juce::jlimit(0.0, 1.0, color01) + sign * 0.15 * skew);

                if (std::abs(p - sd.lastPlexus) > 1.0e-4)
                {
                    sd.matrix.setAmount(p);   // block-rate, only when it actually moved
                    sd.lastPlexus = p;
                }

                // Active lines: line 0 always; line l fades in around PLEXUS = l/8.
                double wSum = 0.0;
                for (int l = 0; l < kNumLines; ++l)
                {
                    const double c = (double) l / (double) kNumLines;
                    sd.w[l] = l == 0 ? 1.0 : smoothstep(c - 0.1, c + 0.1, p);
                    wSum += sd.w[l];
                }
                sd.outGain = kOutGain / std::sqrt(juce::jmax(1.0, wSum));

                // Lengths: rhythmic -> irregular, echo character held over the lower third.
                const double t = std::pow(p, 1.5);
                const double sizeSamples = (1.0 + 499.0 * sz) * sampleRate * 0.0029411765;
                for (int l = 0; l < kNumLines; ++l)
                {
                    const double frac = kLineFracEcho[l] + (kLineFrac[l] - kLineFracEcho[l]) * t;
                    // Whole samples: the interpolated read is only fractional *while gliding* --
                    // once a line settles it reads exactly. (A settled fractional read is a mild
                    // low-pass, and inside the loop it compounds: every repeat got duller,
                    // whatever COLOR said. The smoke test's COLOR check caught it.)
                    sd.targetDelay[l] = juce::jmax(1.0, std::round(frac * sizeSamples));
                    if (snapDelays)
                        sd.curDelay[l] = sd.targetDelay[l];
                }

                // Diffusion only smears from ~15% up; below, DIFFUSE softens each repeat instead.
                const double tie = smoothstep(0.15, 0.7, p);
                sd.diffusionGain = diffuse01 * 0.75 * tie;
                sd.dampCoeff     = diffuse01 * (0.55 + (0.3 - 0.55) * tie);

                // COLOR: cut-only tilt (-1.5 dB max per pass + slow motion); anticlockwise cuts
                // highs (each repeat darker), clockwise cuts lows (each repeat brighter).
                const double signedColor = (co - 0.5) * 2.0;
                sd.colorPhase += sd.colorRateHz * ((double) juce::jmax(0, blockNumSamples) / sampleRate)
                                 * juce::MathConstants<double>::twoPi;
                if (sd.colorPhase > juce::MathConstants<double>::twoPi)
                    sd.colorPhase = std::fmod(sd.colorPhase, juce::MathConstants<double>::twoPi);
                double g = 1.5 * signedColor + 0.75 * std::abs(signedColor) * std::sin(sd.colorPhase);
                if (signedColor > 0.0) g = juce::jmax(0.0, g);   // motion never flips the tilt's side
                if (signedColor < 0.0) g = juce::jmin(0.0, g);
                const double hiCutDb = juce::jmax(0.0, -g), loCutDb = juce::jmax(0.0, g);
                for (int l = 0; l < kNumLines; ++l)
                {
                    setHighShelfCoeffs(sd.colorHi[l], kColorShelfHz, kColorShelfQ, -hiCutDb, sampleRate);
                    setLowShelfCoeffs (sd.colorLo[l], kColorShelfHz, kColorShelfQ, -loCutDb, sampleRate);
                }
            }
            snapDelays = false;
        }

        inline void processSample(float inL, float inR, float& outL, float& outR) noexcept
        {
            const float in[2] = { inL * (float) levelGain, inR * (float) levelGain };
            float fdn[2][kNumLines], mix[2][kNumLines];

            for (int s = 0; s < 2; ++s)
            {
                auto& sd = side[s];
                float mIn[kNumLines];
                for (int l = 0; l < kNumLines; ++l)
                {
                    const double gap = sd.targetDelay[l] - sd.curDelay[l];
                    sd.curDelay[l] = std::abs(gap) < 1.0e-3 ? sd.targetDelay[l] : sd.curDelay[l] + gap * delaySlew;
                    float y = sd.lines[l].readInterpolated(sd.curDelay[l]);
                    y = sd.colorHi[l].processSample(y);
                    y = sd.colorLo[l].processSample(y);
                    y = chebyshevPerturb(y, driveAmt, sd.saturatorHp[l]);
                    fdn[s][l] = y;
                    mIn[l] = y * (float) decayGain;
                }
                sd.matrix.apply(mIn, mix[s]);
            }

            // COUPLE: rotate each L line into its R partner (energy-preserving).
            for (int l = 0; l < kNumLines; ++l)
            {
                const float a = mix[0][l], b = mix[1][l];
                mix[0][l] = (float) ( coupleCos * a + coupleSin * b);
                mix[1][l] = (float) (-coupleSin * a + coupleCos * b);
            }

            float wet[2];
            for (int s = 0; s < 2; ++s)
            {
                auto& sd = side[s];
                double sum = 0.0;
                for (int l = 0; l < kNumLines; ++l)
                {
                    double branch = (double) mix[s][l] + (double) in[s] * sd.w[l];
                    branch = sd.damping[l].process(branch, sd.dampCoeff);
                    float x = (float) branch;
                    for (int a = 0; a < kAllpassesPerLine; ++a)
                        x = sd.allpass[l][a].process(x, allpassDelay[l][a], sd.diffusionGain);
                    sd.lines[l].write(x);
                    sum += sd.w[l] * (double) fdn[s][l];
                }
                wet[s] = (float) (sum * sd.outGain);
            }

            outL = juce::jlimit(-4.0f, 4.0f, (float) (mixDry * inL + mixWet * wet[0]));
            outR = juce::jlimit(-4.0f, 4.0f, (float) (mixDry * inR + mixWet * wet[1]));
        }
    };
}
