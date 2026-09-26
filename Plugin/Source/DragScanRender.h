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

    // Renders one block. The loop region moves linearly from [startA, endA) at the block's first
    // sample to [startB, endB) at its end. `maxFadeLen` is the crossfade length in samples (0 =
    // off), clamped per sample to half the current region. Returns false if a non-looping region
    // ran out (the rest of the block is left untouched -- the caller has already cleared it).
    inline bool renderBlock(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                            const juce::AudioBuffer<float>& docBuf, double& pos,
                            double startA, double endA, double startB, double endB,
                            bool loop, double maxFadeLen, double sampleRate)
    {
        const int64_t docLen = docBuf.getNumSamples();
        const int srcChans = docBuf.getNumChannels();
        if (docLen <= 1 || srcChans <= 0 || numSamples <= 0)
            return true;

        // Kept as a fixed, gentle constant rather than raised for a short loop -- a much higher
        // proportional gain closes the gap faster on paper, but turns this into a near-bang-bang
        // response that amplifies ordinary drag-input jitter into audible chatter (tried and
        // reverted -- see processBlock's maxSlewSpeed comment for the actual small-loop fix,
        // which throttles the *window's* own speed instead and leaves this untouched). Must
        // match processBlock's own catchUpGainPerSec.
        constexpr double catchUpGainPerSec = 8.0;
        // Backward drags never actually need the catch-up branch: the playhead already sits at
        // the *far* edge from a retreating end, comfortably inside the window at plain 1x. Forward
        // drags are the opposite -- zero margin at the low edge means catch-up runs for as long
        // as the window keeps advancing, so this cap *is* the forward-drag pitch you hear. Keep
        // it a little above processBlock's region-slew cap so the gap can actually close.
        const double maxCatchUpSpeed = sampleRate * 2.0;
        const double sr = juce::jmax(1.0, sampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            const double a = (double) (i + 1) / (double) numSamples;
            const double regionStart = startA + (startB - startA) * a;
            const double regionEnd   = juce::jmax(regionStart + 1.0, endA + (endB - endA) * a);
            const double regionLen   = regionEnd - regionStart;

            double velocitySamplesPerSec;
            if (pos < regionStart)
                velocitySamplesPerSec = juce::jlimit(sampleRate, maxCatchUpSpeed,
                                                     (regionStart - pos) * catchUpGainPerSec);
            else if (pos >= regionEnd)
                velocitySamplesPerSec = maxCatchUpSpeed;   // overshot a retreating end -- race to the wrap
            else
                velocitySamplesPerSec = sampleRate;        // inside the window: plain 1x

            if (pos >= 0.0 && pos < (double) (docLen - 1))
            {
                const int64_t i0 = (int64_t) pos;
                const float frac = (float) (pos - (double) i0);
                const double fadeLen = loop ? juce::jmin(maxFadeLen, regionLen * 0.5) : 0.0;
                const float g = (float) edgeFadeGain(pos - regionStart, regionLen, fadeLen);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    const float* d = docBuf.getReadPointer(juce::jmin(ch, srcChans - 1));
                    out.setSample(ch, i, (d[i0] + (d[i0 + 1] - d[i0]) * frac) * g);
                }
            }
            pos += velocitySamplesPerSec / sr;

            if (pos >= regionEnd)
            {
                if (! loop)
                    return false;
                pos -= regionLen;   // keeps the fractional remainder -- continuous across the wrap
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
                                  const juce::AudioBuffer<float>& docBuf, double pos,
                                  double regionStart, double regionEnd,
                                  bool loop, double maxFadeLen, double sampleRate)
    {
        tail.setSize(numCh, len, false, false, true);
        tail.clear();
        renderBlock(tail, numCh, len, docBuf, pos, regionStart, regionEnd, regionStart, regionEnd,
                    loop, maxFadeLen, sampleRate);
    }
}
