#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// CHORUS: a BBD-style chorus right after the filter (before RTRG). Built from CHORUS_PLAN.md
// (repo root) -- read that first before changing a constant; it carries the "why" and the
// measured numbers. References: pendragon-andyh/Juno60 Chorus README (measured LFO rates and
// delay ranges), the KVR "Juno 60 chorus" thread, Gearspace "Detailed values of the Juno-106
// chorus", GroupDIY "Roland Juno Chorus by TC", the Juno-106 owner's manual. (Reference names
// stay out of the UI -- it's CHO / CHORUS there.)
//
//   MODE   -- I / II / I+II: the classic three-button chorus. I and II are slow triangle sweeps
//             over 1.66..5.35 ms; I+II is its own fast, shallow 9.75 Hz shimmer, not I and II
//             summed.
//   METAL  -- 0 = the classic chorus, untouched. Up: the delay shrinks toward 0.5 ms, the sweep
//             narrows, feedback rises to 0.92 and the BBD filter opens -- a resonant, pitched
//             comb ring (flanger / metallic).
//   popup  -- MIX (0.5 = equal dry + wet), RATE trim (x0.5..x2), WIDTH (R channel's LFO phase
//             offset, 1 = inverted), HISS (BBD noise, 0 = bit-clean), RING +/- (feedback sign).
//
// Per channel: in (+ feedback + hiss) -> 2-pole LP -> soft-sat -> delay line -> Hermite read =
// wet; the feedback tap is the wet through an 80 Hz HP (the comb's DC peak would otherwise be
// 12.5x). Enabling/disabling crossfades against the input; once faded out, process() leaves the
// buffer untouched (bit-exact) -- busy() says when that's reached. Live playback only, not baked
// into Save/Export (same as the other drawer FX). Header-only, plain doubles, no allocation after
// prepare() -- the smoke test drives exactly this code.
namespace r3wrk
{
struct ChorusEngine
{
    static constexpr int    kMaxChannels = 2;
    static constexpr double kMaxDelayMs  = 20.0;
    static constexpr double kTrim        = 0.75;

    struct Params
    {
        bool   enabled = false;
        double mode01 = 0.0, metal01 = 0.0, mix01 = 0.5, rate01 = 0.5, width01 = 1.0, hiss01 = 0.0;
        bool   ringNegative = false;
    };

    static int modeIndex(double mode01) noexcept
    {
        return juce::jlimit(0, 2, (int) std::floor(juce::jlimit(0.0, 1.0, mode01) * 3.0 * 0.9999));
    }
    static const char* modeName(int idx) noexcept
    {
        static const char* names[] = { "I", "II", "I+II" };
        return names[juce::jlimit(0, 2, idx)];
    }
    static double rateHz(int idx, double rate01) noexcept
    {
        static constexpr double base[] = { 0.513, 0.863, 9.75 };   // measured (CHORUS_PLAN §1.1)
        return base[juce::jlimit(0, 2, idx)] * std::pow(2.0, (juce::jlimit(0.0, 1.0, rate01) - 0.5) * 2.0);
    }
    // The delay sweep (ms) for a mode + METAL: centre +- depth.
    static double centreMs(int idx, double metal01) noexcept
    {
        const double c0 = idx < 2 ? 3.505 : 3.5;
        return c0 * std::pow(0.5 / c0, juce::jlimit(0.0, 1.0, metal01));
    }
    static double depthMs(int idx, double metal01) noexcept
    {
        const double m = juce::jlimit(0.0, 1.0, metal01);
        const double c0 = idx < 2 ? 3.505 : 3.5, d0 = idx < 2 ? 1.845 : 0.2;
        return d0 * (centreMs(idx, m) / c0) * (1.0 - 0.6 * m);
    }
    static double feedback(double metal01, bool ringNegative) noexcept
    {
        return 0.92 * std::pow(juce::jlimit(0.0, 1.0, metal01), 1.2) * (ringNegative ? -1.0 : 1.0);
    }
    static double filterHz(double metal01) noexcept
    {
        return 9000.0 * std::pow(16000.0 / 9000.0, juce::jlimit(0.0, 1.0, metal01));
    }

    void prepare(double sampleRate, int numChannels)
    {
        sr = juce::jmax(8000.0, sampleRate);
        channels = juce::jlimit(1, kMaxChannels, numChannels);
        lineLen = (int) std::ceil(sr * kMaxDelayMs * 0.001) + 8;
        for (auto& ch : ch_)
            ch.line.assign((size_t) lineLen, 0.0f);
        const auto coef = [this](double tauSec) { return 1.0 - std::exp(-1.0 / (tauSec * sr)); };
        kFast   = coef(0.030);
        kCentre = coef(0.050);
        fadeInStep  = 1.0 / (0.010 * sr);
        fadeOutStep = 1.0 / (0.020 * sr);
        hpA = 1.0 / (1.0 + 2.0 * juce::MathConstants<double>::pi * 80.0 / sr);
        reset();
    }

    // Clears the lines, filters and fade, LFO phase back to 0.
    void reset() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            auto& ch = ch_[c];
            std::fill(ch.line.begin(), ch.line.end(), 0.0f);
            ch.lpX1 = ch.lpX2 = ch.lpY1 = ch.lpY2 = 0.0;
            ch.hpX1 = ch.hpY1 = 0.0;
            ch.pink = 0.0;
            ch.rng = c == 0 ? 0x9E3779B9u : 0x7F4A7C15u;
            ch.lastDelayMs = 0.0;
        }
        writePos = 0;
        phase = 0.0;
        fade = 0.0;
        cachedFc = -1.0;
    }

    // Jump every smoother to its target (offline tests / the enable edge) instead of gliding.
    void snapToTargets(const Params& p) noexcept
    {
        const Targets t = targetsFor(p);
        centre = t.centre; depth = t.depth; fb = t.fb; rate = t.rate;
        dryGain = t.dry; wetGain = t.wet; phaseOffR = t.phaseOffR; fc = t.fc;
        updateFilter(true);
    }

    // Still fading out (or in) -- keep calling process() until this goes false.
    bool busy() const noexcept { return fade > 0.0; }

    // In place, numCh 1 or 2 (mono runs the L path only).
    void process(float* const* io, int numCh, int n, const Params& p) noexcept
    {
        if (! p.enabled && fade <= 0.0)
            return;   // fully bypassed -- the buffer is untouched
        numCh = juce::jmin(numCh, channels);
        const Targets t = targetsFor(p);
        const double hissAmp = p.hiss01 > 0.0 ? p.hiss01 * p.hiss01 * 0.001 : 0.0;
        const double maxDelay = (double) lineLen - 4.0;

        for (int i = 0; i < n; ++i)
        {
            centre    += kCentre * (t.centre - centre);
            depth     += kCentre * (t.depth - depth);
            fb        += kFast * (t.fb - fb);
            rate      += kFast * (t.rate - rate);
            dryGain   += kFast * (t.dry - dryGain);
            wetGain   += kFast * (t.wet - wetGain);
            phaseOffR += kFast * (t.phaseOffR - phaseOffR);
            fc        += kFast * (t.fc - fc);
            updateFilter(false);
            fade = p.enabled ? juce::jmin(1.0, fade + fadeInStep) : juce::jmax(0.0, fade - fadeOutStep);

            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = ch_[c];
                double ph = phase + (c == 1 ? phaseOffR : 0.0);
                ph -= std::floor(ph);
                const double tri = 4.0 * std::abs(ph - 0.5) - 1.0;
                const double dMs = centre + depth * tri;
                ch.lastDelayMs = dMs;
                const double wet = readHermite(ch, juce::jlimit(3.0, maxDelay, dMs * 0.001 * sr));

                // Feedback tap through the 80 Hz HP (kills the comb's DC peak).
                const double hp = hpA * (ch.hpY1 + wet - ch.hpX1);
                ch.hpX1 = wet; ch.hpY1 = hp;

                const double in = io[c][i];
                double x = in + fb * hp;
                if (hissAmp > 0.0)
                    x += hissAmp * nextPink(ch);
                // 2-pole LP (BBD anti-alias / reconstruction), then the soft saturator.
                const double y = b0 * x + b1 * ch.lpX1 + b2 * ch.lpX2 - a1 * ch.lpY1 - a2 * ch.lpY2;
                ch.lpX2 = ch.lpX1; ch.lpX1 = x;
                ch.lpY2 = ch.lpY1; ch.lpY1 = y;
                ch.line[(size_t) writePos] = (float) sat(y);

                const double out = kTrim * (dryGain * in + wetGain * wet);
                io[c][i] = (float) (in + fade * (out - in));
            }
            writePos = (writePos + 1) % lineLen;
            if (! lfoFrozen)
            {
                phase += rate / sr;
                phase -= std::floor(phase);
            }
        }

        // Self-heal: a non-finite state would otherwise live in the feedback loop forever.
        bool finite = std::isfinite(centre) && std::isfinite(fb) && std::isfinite(fc);
        for (int c = 0; c < numCh; ++c)
            finite = finite && std::isfinite(ch_[c].lpY1) && std::isfinite(ch_[c].lpY2) && std::isfinite(ch_[c].hpY1);
        if (! finite)
        {
            const double f = fade;
            reset();
            snapToTargets(p);
            fade = f;
        }
    }

    // Readback for the smoke test / UI.
    double currentCentreMs() const noexcept { return centre; }
    double currentDepthMs() const noexcept { return depth; }
    double currentFeedback() const noexcept { return fb; }
    double currentDelayMs(int c) const noexcept { return ch_[juce::jlimit(0, kMaxChannels - 1, c)].lastDelayMs; }

    // Test-only: hold the LFO still (at `setLfoPhase`) so the comb can be measured.
    bool lfoFrozen = false;
    void setLfoPhase(double p) noexcept { phase = p - std::floor(p); }

private:
    struct Targets { double centre, depth, fb, rate, dry, wet, phaseOffR, fc; };

    Targets targetsFor(const Params& p) const noexcept
    {
        const int idx = modeIndex(p.mode01);
        const double m = juce::jlimit(0.0, 1.0, p.metal01), mix = juce::jlimit(0.0, 1.0, p.mix01);
        Targets t;
        t.centre = centreMs(idx, m);
        t.depth  = depthMs(idx, m);
        t.fb     = feedback(m, p.ringNegative);
        t.rate   = rateHz(idx, p.rate01);
        t.dry    = juce::jmin(1.0, 2.0 * (1.0 - mix));
        t.wet    = juce::jmin(1.0, 2.0 * mix) * std::sqrt(1.0 - std::abs(t.fb));   // resonance compensation
        t.phaseOffR = 0.5 * juce::jlimit(0.0, 1.0, p.width01);
        t.fc     = juce::jmin(filterHz(m), 0.45 * sr);
        return t;
    }

    // RBJ lowpass, Q 0.707 -- recomputed only once fc has moved ~0.5%.
    void updateFilter(bool force) noexcept
    {
        if (! force && std::abs(fc - cachedFc) <= cachedFc * 0.005)
            return;
        cachedFc = fc;
        const double w0 = 2.0 * juce::MathConstants<double>::pi * fc / sr;
        const double cs = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 - cs) * 0.5 / a0; b1 = (1.0 - cs) / a0; b2 = b0;
        a1 = -2.0 * cs / a0;        a2 = (1.0 - alpha) / a0;
    }

    // Linear to 0.5, then a smooth knee into +-1.2 ("rounded sawtooth"; bounds the loop).
    static double sat(double x) noexcept
    {
        const double a = std::abs(x);
        if (a <= 0.5) return x;
        const double y = 0.5 + 0.7 * std::tanh((a - 0.5) / 0.7);
        return x < 0.0 ? -y : y;
    }

    struct Channel
    {
        std::vector<float> line;
        double lpX1 = 0, lpX2 = 0, lpY1 = 0, lpY2 = 0, hpX1 = 0, hpY1 = 0, pink = 0, lastDelayMs = 0;
        uint32_t rng = 1;
    };

    // 4-point Hermite (Catmull-Rom) read `d` samples back from the next write slot.
    double readHermite(const Channel& ch, double d) const noexcept
    {
        const double pos = (double) writePos - d;
        const double fl = std::floor(pos);
        const double f = pos - fl;
        int i0 = (int) fl % lineLen;
        if (i0 < 0) i0 += lineLen;
        const auto at = [&](int k) { return (double) ch.line[(size_t) ((i0 + k + lineLen) % lineLen)]; };
        const double ym1 = at(-1), y0 = at(0), y1 = at(1), y2 = at(2);
        const double c1 = 0.5 * (y1 - ym1);
        const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    // xorshift32 white (unit RMS), tilted toward pink with one one-pole.
    static double nextPink(Channel& ch) noexcept
    {
        uint32_t x = ch.rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ch.rng = x;
        const double white = ((double) x / 4294967295.0 * 2.0 - 1.0) * 1.7320508;
        ch.pink = 0.5 * ch.pink + 0.5 * white;
        return ch.pink * 1.7320508;   // back to ~unit RMS
    }

    double sr = 44100.0;
    int channels = 1, lineLen = 1, writePos = 0;
    Channel ch_[kMaxChannels];
    double phase = 0.0, fade = 0.0;
    double centre = 3.505, depth = 1.845, fb = 0.0, rate = 0.513, dryGain = 1.0, wetGain = 1.0, phaseOffR = 0.5, fc = 9000.0;
    double kFast = 0.001, kCentre = 0.001, fadeInStep = 0.001, fadeOutStep = 0.001, hpA = 0.99;
    double cachedFc = -1.0, b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
}
