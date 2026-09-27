#pragma once
#include <JuceHeader.h>
#include <cmath>

// "Dirt": the pre-filter harmonics stage -- Octatrack-style drive, then sample-rate reduction,
// then bit reduction, ahead of the Base/Width filter (so the filter shapes the harmonics, the way
// the OT's filter DIST does). The Dirt knob in KnobRow is Drive; its "···" popup (DirtPanel) adds
// Rate and Bits. Everything at default = a bit-exact bypass (callers skip it entirely).
//
//   Drive -- the OT manual describes its filter DIST only as "sets the headroom of the filter --
//            higher value = lower headroom": push harder into a fixed ceiling. So: pre-gain
//            1x..30x (exponential feel) into a clipper that's linear to 0.8 and then knees hard
//            into a ceiling -- gritty and digital rather than a warm tanh. Even harmonics come
//            from a small bias on the *input* (0.03 x Drive, before the gain): it moves the zero
//            crossings, so the clipped halves have different lengths, like a biased tube stage.
//            (Measured in the smoke test: a bias *after* the gain, or unequal positive/negative
//            ceilings, both left 2f at 1.7% -- a hard-clipped wave is nearly square, and a square
//            with equal-length halves has only odd harmonics whatever its heights.) The negative
//            ceiling is still lower (0.7 at full Drive) for a lopsided top. Side effect, fuzz-
//            like: at full Drive, audio quieter than the bias gets gated and sputters.
//            Partial makeup (gain^-0.5): turning it up adds grit more than level. No
//            oversampling -- the aliasing is part of the digital grit. Only the *added* part
//            (distorted - dry) goes through the DC blocker, so the asymmetry's offset is removed while
//            the dry signal passes untouched and Drive can fade in from 0 without a filter
//            switching on.
//   Rate  -- sample-and-hold, log from off (1.0) down to ~1 kHz, no anti-aliasing.
//   Bits  -- quantise to 2..16 bits (off at 1.0), no dither.
//
// Header-only, per channel, plain doubles, no allocation -- the codebase's DSP convention --
// so the smoke test runs exactly this code.
namespace r3wrk
{
struct DirtStage
{
    static bool engaged(double drive01, double rate01, double bits01) noexcept
    {
        return drive01 > 1.0e-4 || rate01 < 0.999 || bits01 < 0.999;
    }

    // Sample-and-hold rate for the Rate knob; returns 0 when off.
    static double rateHz(double rate01, double sampleRate) noexcept
    {
        if (rate01 >= 0.999) return 0.0;
        rate01 = juce::jlimit(0.0, 1.0, rate01);
        return 1000.0 * std::pow(juce::jmax(1000.0, sampleRate) / 1000.0, rate01);
    }

    // Bit depth for the Bits knob; returns 0 when off.
    static int bitDepth(double bits01) noexcept
    {
        if (bits01 >= 0.999) return 0;
        return 2 + (int) std::lround(juce::jlimit(0.0, 1.0, bits01) * 14.0);
    }

    void prepare(double sampleRate) noexcept { sr = juce::jmax(1000.0, sampleRate); reset(); }

    void reset() noexcept
    {
        drive = 0.0; hpX1 = hpY1 = 0.0; held = 0.0f; holdPhase = 1.0;
    }

    // Jump straight to a Drive value (offline bake / a fresh play pass) instead of ramping.
    void snapDrive(double drive01) noexcept { drive = juce::jlimit(0.0, 1.0, drive01); }

    // Still has something to do even though the knobs are at default (Drive ramping down, or
    // the DC blocker's tail decaying) -- keep calling process() until this goes false.
    bool busy() const noexcept { return drive > 0.0 || std::abs(hpY1) > 1.0e-7 || std::abs(hpX1) > 1.0e-7; }

    void process(float* x, int n, double driveTarget, double rate01, double bits01) noexcept
    {
        driveTarget = juce::jlimit(0.0, 1.0, driveTarget);
        const double maxStep = 1.0 / (0.02 * sr);   // full range in 20 ms -- no zipper
        const double holdInc = rateHz(rate01, sr) / sr;
        const int bits = bitDepth(bits01);
        const double levels = bits > 0 ? std::ldexp(1.0, bits - 1) : 0.0;
        const double r = 1.0 - (2.0 * juce::MathConstants<double>::pi * 20.0 / sr);   // ~20 Hz DC blocker

        for (int i = 0; i < n; ++i)
        {
            drive += juce::jlimit(-maxStep, maxStep, driveTarget - drive);
            if (drive < 1.0e-9) drive = 0.0;

            double y = x[i];
            if (drive > 0.0 || hpY1 != 0.0 || hpX1 != 0.0)
            {
                double added = 0.0;
                if (drive > 0.0)
                {
                    const double g = std::pow(30.0, drive);
                    const double bias = 0.03 * drive, neg = 1.0 - 0.3 * drive;
                    const double wet = (clip((y + bias) * g, neg) - clip(bias * g, neg)) * std::pow(g, -0.5);
                    added = wet - y;
                }
                // DC-block only what the drive added (see the header comment).
                const double hp = added - hpX1 + r * hpY1;
                hpX1 = added;
                hpY1 = std::abs(hp) < 1.0e-12 ? 0.0 : hp;
                if (drive == 0.0 && std::abs(hpY1) < 1.0e-9 && std::abs(hpX1) < 1.0e-9)
                    hpX1 = hpY1 = 0.0;
                y += hpY1;
            }

            if (holdInc > 0.0)
            {
                holdPhase += holdInc;
                if (holdPhase >= 1.0)
                {
                    holdPhase -= std::floor(holdPhase);
                    held = (float) y;
                }
                y = held;
            }
            if (levels > 0.0)
                y = std::round(y * levels) / levels;

            x[i] = (float) juce::jlimit(-2.0, 2.0, y);
        }
    }

private:
    // Positive half: linear to 0.8, then a hard knee into 1.0. Negative half: the same shape
    // scaled to `negCeiling` (linear to 0.8 * negCeiling, knee into negCeiling).
    static double clip(double v, double negCeiling) noexcept
    {
        const double ceil = v < 0.0 ? negCeiling : 1.0;
        const double knee = 0.8 * ceil, span = 0.2 * ceil;
        const double a = std::abs(v);
        const double out = a <= knee ? a : knee + span * std::tanh((a - knee) / span);
        return v < 0.0 ? -out : out;
    }

    double sr = 44100.0, drive = 0.0, hpX1 = 0.0, hpY1 = 0.0, holdPhase = 1.0;
    float held = 0.0f;
};
}
