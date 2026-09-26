#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

// The drag-scan renderer (R3WRKAudioProcessor::renderDragScan's body), pulled out as plain
// functions so Tests/SmokeTest.cpp can drive the exact code the audio thread runs.
//
// Why the loop edges are interpolated per sample: while a loop edge is being dragged,
// processBlock slews the region once per block, so an edge can move hundreds of samples between
// one block and the next. The loop-crossfade envelope is measured from those edges, so a playhead
// sitting inside a fade zone saw its gain step at every block boundary (e.g. 0.01 -> 0.56 in one
// sample) -- a click whose loudness is whatever the material is doing at that instant, which is
// why it only crackled on some sounds and some drags (worst on short loops, which spend more of
// their time near an edge). Moving the edges smoothly across the block makes the envelope move
// smoothly too.
//
// Why the envelope is distance-to-the-nearest-edge from *either* side: the old envelope gave any
// position outside [start, end) full gain and one just inside an edge ~zero, so a playhead
// crossing an edge -- catching up into the window, or overshooting a retreating end by a fraction
// of a sample -- flipped between 1 and 0 in one sample. Measuring distance from outside as well
// keeps the gain at 0 on both sides of every edge, so crossing one (or wrapping) is silent.
namespace dragscan
{
    // Raised-cosine (equal-power) loop-edge fade -- 0 at either edge of [0, regionLen), rising to
    // 1 at `fadeLen` samples from the nearest edge, and symmetrically back down outside it.
    inline double edgeFadeGain(double rp, double regionLen, double fadeLen) noexcept
    {
        if (fadeLen <= 0.0)
            return 1.0;
        const double last = regionLen - 1.0;
        const double d = rp < 0.0  ? -rp
                       : rp > last ? rp - last
                                   : std::min(rp, last - rp);
        const double x = juce::jlimit(0.0, 1.0, d / fadeLen);
        const double s = std::sin(0.5 * juce::MathConstants<double>::pi * x);
        return s * s;
    }

    // How fast the dragged loop window may slide, in source samples per second. Capped just below
    // the playhead's own read speed -- 1x of the source, i.e. sampleRate / timeRatio when the
    // file is time-stretched -- so the window can never outrun the playhead: it reads at a
    // constant 1x (no pitch rise) and simply loops inside a window that's gaining on it more
    // slowly than it moves. (At the old 1.5x cap a fast forward Start drag overtook the
    // playhead continuously; at 1x reading that pinned it at the loop seam -- 91% silent in
    // the smoke test -- and the only alternative was reading faster, i.e. the pitch rise.)
    // Short loops are throttled further (the original small-loop limit, kept for its feel).
    inline double maxWindowSpeed(double sampleRate, double timeRatio, double targetRegionLen) noexcept
    {
        const double playRate = sampleRate / juce::jmax(1.0e-4, timeRatio);
        constexpr double gainPerSec = 8.0, maxTrailingFractionOfLoop = 0.35;
        return juce::jlimit(playRate * 0.1, playRate * 0.95,
                            targetRegionLen * gainPerSec * maxTrailingFractionOfLoop);
    }

    // Crossfade state for a relocation (see renderBlock), carried across blocks alongside `pos`.
    struct Relocation
    {
        double oldPos = 0.0;     // where the playhead was -- keeps playing, fading out, at 1x
        float  oldGain = 0.0f;   // its loop-edge gain at the moment it was left (frozen)
        int    remaining = 0, len = 0;
    };

    // `pos` folded into [start, start + len) by whole loop lengths -- the same point in the loop's
    // cycle, just inside the window.
    inline double wrapInto(double pos, double start, double len) noexcept
    {
        double r = std::fmod(pos - start, len);
        if (r < 0.0) r += len;
        return start + r;
    }

    // Renders one block, always reading at exactly 1x -- the pitch never changes. The loop region
    // moves linearly from [startA, endA) at the block's first sample to [startB, endB) at its end.
    // `maxFadeLen` is the loop crossfade length in samples (0 = off), clamped per sample to half
    // the current region. Returns false if a non-looping region ran out (the rest of the block is
    // left untouched -- the caller has already cleared it).
    //
    // When the moving window passes the playhead (its start edge overtakes it, a retreating end
    // edge crosses it, or the whole loop jumps elsewhere), the playhead *relocates* to the same
    // point in the loop's cycle inside the window, with a short equal-power crossfade from where
    // it was. This used to be a catch-up instead -- reading up to 2x fast until the playhead got
    // back inside -- which is what made a dragged loop sometimes rise in pitch. In the common
    // case (an edge passing the playhead) both sides of the relocation sit at an edge, where the
    // loop fade is ~0, so it's as silent as the ordinary loop seam; the crossfade covers the rest
    // (a big jump from mid-loop).
    inline bool renderBlock(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                            const juce::AudioBuffer<float>& docBuf, double& pos, Relocation& rel,
                            double startA, double endA, double startB, double endB,
                            bool loop, double maxFadeLen, double sampleRate)
    {
        const int64_t docLen = docBuf.getNumSamples();
        const int srcChans = docBuf.getNumChannels();
        if (docLen <= 1 || srcChans <= 0 || numSamples <= 0)
            return true;

        const int relocLen = juce::jmax(1, (int) std::lround(0.010 * sampleRate));
        auto read = [&](double p, int ch) -> float
        {
            if (p < 0.0 || p >= (double) (docLen - 1))
                return 0.0f;
            const int64_t i0 = (int64_t) p;
            const float frac = (float) (p - (double) i0);
            const float* d = docBuf.getReadPointer(juce::jmin(ch, srcChans - 1));
            return d[i0] + (d[i0 + 1] - d[i0]) * frac;
        };

        for (int i = 0; i < numSamples; ++i)
        {
            const double a = (double) (i + 1) / (double) numSamples;
            const double regionStart = startA + (startB - startA) * a;
            const double regionEnd   = juce::jmax(regionStart + 1.0, endA + (endB - endA) * a);
            const double regionLen   = regionEnd - regionStart;
            const double fadeLen = loop ? juce::jmin(maxFadeLen, regionLen * 0.5) : 0.0;

            if (pos < regionStart || pos >= regionEnd)
            {
                double target;
                if (loop)                    target = wrapInto(pos, regionStart, regionLen);
                else if (pos < regionStart)  target = regionStart;
                else                         return false;   // not looping, past the end
                rel.oldPos = pos;
                rel.oldGain = (float) edgeFadeGain(pos - regionStart, regionLen, fadeLen);
                rel.len = rel.remaining = relocLen;
                pos = target;
            }

            const float g = (float) edgeFadeGain(pos - regionStart, regionLen, fadeLen);
            float gIn = 1.0f, gOut = 0.0f;
            if (rel.remaining > 0)
            {
                const double x = ((double) (rel.len - rel.remaining) + 0.5) / (double) rel.len;
                gIn  = (float) std::sin(0.5 * juce::MathConstants<double>::pi * x);
                gOut = (float) std::cos(0.5 * juce::MathConstants<double>::pi * x) * rel.oldGain;
            }
            for (int ch = 0; ch < numCh; ++ch)
            {
                float y = read(pos, ch) * g * gIn;
                if (gOut != 0.0f)
                    y += read(rel.oldPos, ch) * gOut;
                out.setSample(ch, i, y);
            }
            if (rel.remaining > 0)
            {
                rel.oldPos += 1.0;
                --rel.remaining;
            }

            pos += 1.0;
            if (pos >= regionEnd)
            {
                if (! loop)
                    return false;
                pos -= regionLen;   // the ordinary loop seam -- both sides at the edge fade's 0
            }
        }
        return true;
    }

    // What the drag renderer *would* have played next, from `pos` over a region now frozen at
    // [regionStart, regionEnd) -- the "old" side of the crossfade when a drag is released. On
    // release the region snaps to where the mouse left it (the slew was still catching up), and
    // the playhead often lands outside it and gets jumped to the loop start; without an old tail
    // to fade out, whatever was playing just stopped mid-waveform (a click on ~1 release in 3).
    inline void renderReleaseTail(juce::AudioBuffer<float>& tail, int numCh, int len,
                                  const juce::AudioBuffer<float>& docBuf, double pos, Relocation rel,
                                  double regionStart, double regionEnd,
                                  bool loop, double maxFadeLen, double sampleRate)
    {
        tail.setSize(numCh, len, false, false, true);
        tail.clear();
        renderBlock(tail, numCh, len, docBuf, pos, rel, regionStart, regionEnd, regionStart, regionEnd,
                    loop, maxFadeLen, sampleRate);
    }
}
