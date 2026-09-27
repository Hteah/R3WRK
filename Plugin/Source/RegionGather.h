#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

// The loop reader PluginProcessor's ordinary playback path uses (moved here unchanged from
// PluginProcessor.cpp, apart from being `inline` instead of file-`static`, so the smoke test can
// drive exactly this code -- see its reverse / ping-pong range checks).
namespace r3wrk
{
// Loop-crossfade envelope: raised-cosine (equal-power) gain for region-relative frame `rp`
// -- ramps 0->1 over the first `fadeLen` frames of the region and 1->0 over the last
// `fadeLen`, else 1. `fadeLen` is pre-clamped to regionLen/2 by the caller.
inline double loopFadeGain(int64_t rp, int64_t regionLen, int fadeLen) noexcept
{
    if (fadeLen <= 0) return 1.0;
    double x;
    if (rp < fadeLen)                        x = (double) rp / (double) fadeLen;
    else if (rp >= regionLen - fadeLen)      x = (double) (regionLen - 1 - rp) / (double) fadeLen;
    else                                     return 1.0;
    x = juce::jlimit(0.0, 1.0, x);
    const double s = std::sin(0.5 * juce::MathConstants<double>::pi * x);
    return s * s;
}

// Walks [regionStart, regionEnd) from `pos` in direction `dir` (+1 forward, -1 backward),
// copying up to `count` frames into dst[dstOffset..]. `loop` wraps head-to-tail; `pingPong`
// bounces at both ends instead -- `dir` flips, and neither endpoint is repeated, so the cycle
// is 0,1,..,L-1,L-2,..,1 with period 2L-2 (the same shape as the Sieve editor's ping-pong).
// `dir` only ever goes -1 when `pingPong` is true. `fadeLen` > 0 applies the loop-crossfade
// envelope to the copied audio by its region position. Returns frames written; a short
// return means a non-looping region ran out.
inline int gatherRegion(juce::AudioBuffer<float>& dst, int dstOffset, int count, int dstChannels,
                        const juce::AudioBuffer<float>& docBuf,
                        int64_t& pos, int& dir,
                        int64_t regionStart, int64_t regionEnd, bool loop, bool pingPong,
                        bool reverseLoop, int fadeLen, int64_t* posTrace = nullptr)
{
    const int srcChans = docBuf.getNumChannels();
    if (srcChans <= 0 || regionEnd <= regionStart)
        return 0;
    const int64_t regionLen = regionEnd - regionStart;

    auto applyFade = [&](int atFrame, int chunk, int64_t rp0, int step)
    {
        if (fadeLen <= 0) return;
        float* wp[8];
        const int nw = juce::jmin(dstChannels, 8);
        for (int ch = 0; ch < nw; ++ch) wp[ch] = dst.getWritePointer(ch, dstOffset + atFrame);
        for (int j = 0; j < chunk; ++j)
        {
            const double g = loopFadeGain(rp0 + (int64_t) step * j, regionLen, fadeLen);
            if (g < 1.0)
                for (int ch = 0; ch < nw; ++ch)
                    wp[ch][j] *= (float) g;
        }
    };

    int written = 0;
    while (written < count)
    {
        if (dir > 0)
        {
            if (pos >= regionEnd)
            {
                if (pingPong)    { dir = -1; pos = regionEnd - 2; continue; }   // reflect at the top
                // Reverse-loop mode switched on WHILE playback is already going forward (e.g.
                // pressing Play with nothing selected, then cycling the Loop button mid-playback
                // -- direction otherwise only ever starts backward at a fresh Play press, see
                // processBlock's startReversed) -- flip to backward here too, mirroring the
                // backward branch's own reverseLoop wrap below, instead of silently treating
                // reverseLoop exactly like a plain forward loop until Play is pressed again.
                if (reverseLoop) { dir = -1; pos = regionEnd - 1; continue; }   // the tail's last real sample
                if (loop)        { pos = regionStart; continue; }
                break;
            }
            const int chunk = (int) juce::jmin((int64_t) (count - written), regionEnd - pos);
            for (int ch = 0; ch < dstChannels; ++ch)
                dst.copyFrom(ch, dstOffset + written, docBuf,
                             juce::jmin(ch, srcChans - 1), (int) pos, chunk);
            applyFade(written, chunk, pos - regionStart, +1);
            if (posTrace != nullptr)   // which buffer position each copied frame came from (Overdub)
                for (int j = 0; j < chunk; ++j) posTrace[dstOffset + written + j] = pos + j;
            pos     += chunk;
            written += chunk;
        }
        else   // backward: either the ping-pong return leg (stopping one frame short of
               // regionStart before reflecting forward) or a reverse loop, which instead
               // wraps tail-to-head and keeps playing backward -- the mirror image of a
               // plain forward loop's wrap to regionStart
        {
            // A reverse loop reads down to regionStart itself; ping-pong's return leg stops one
            // frame short of it (the forward leg plays it -- neither endpoint repeats). Reads are
            // [from+1 .. pos], so the floor is the last position NOT read.
            const int64_t floorPos = reverseLoop ? regionStart - 1 : regionStart;
            if (pos <= floorPos)
            {
                if (reverseLoop)
                {
                    // Wrap back to the tail's last real sample, still playing backward. (Was
                    // regionEnd itself -- one past the loop, and past the end of the audio when
                    // the loop runs to the file's end, e.g. after a Trim: an out-of-bounds read.)
                    pos = regionEnd - 1;
                    continue;
                }
                // Ping-pong's return leg bounces back to forward here -- pingPong implies loop
                // (see its own computation at the call site), so this is unreachable with loop
                // off. Mirrors the forward branch's own `if (loop) ... else break` shape: with
                // loop truly off (backward only via a reverse-loop pass that was then switched
                // off mid-playback), there's no mode left to bounce back into -- stop here
                // instead of unconditionally flipping to forward and continuing forever, which
                // is what silently made "loop off" never actually stop once direction had ever
                // gone backward.
                if (loop)
                {
                    dir = 1;
                    pos = regionStart;             // the next frame forward plays
                    continue;
                }
                break;
            }
            const int chunk = (int) juce::jmin((int64_t) (count - written), pos - floorPos);
            const int64_t from = pos - chunk;  // copy [from+1 .. pos] forward, then reverse it
            for (int ch = 0; ch < dstChannels; ++ch)
            {
                dst.copyFrom(ch, dstOffset + written, docBuf,
                             juce::jmin(ch, srcChans - 1), (int) (from + 1), chunk);
                float* w = dst.getWritePointer(ch, dstOffset + written);
                std::reverse(w, w + chunk);
            }
            applyFade(written, chunk, pos - regionStart, -1);   // dst frame 0 == region pos `pos`
            if (posTrace != nullptr)
                for (int j = 0; j < chunk; ++j) posTrace[dstOffset + written + j] = pos - j;
            pos     -= chunk;
            written += chunk;
        }
    }
    return written;
}
}
