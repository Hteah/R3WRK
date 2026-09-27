#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>

// Sound-on-sound overdub: writes live input into the document buffer while the loop plays,
// tape style -- each pass, what's already there is scaled by Feedback and the new input is added
// at Level: doc[p] = doc[p] * feedback + input * level.
//
// Timing: the input arriving now is what you played along to a round-trip latency ago (audio
// out, through the speakers/headphones, back in). So each output sample's buffer position is
// remembered in a history ring as playback reads it (PluginProcessor passes gatherRegion's
// position trace in), and each input sample is written to the position that was playing
// `latency` samples earlier -- exact across loop wraps, ping-pong and reverse, because it's the
// real read order, not a prediction of it.
//
// Header-only, allocation only in prepare(), so the smoke test drives exactly this code.
namespace r3wrk
{
struct OverdubWriter
{
    // Covers round-trip latencies up to ~0.7 s at 44.1 kHz -- far beyond any real interface.
    static constexpr int kHistory = 1 << 15;

    void prepare() { history.assign((size_t) kHistory, -1); reset(); }
    void reset() noexcept { pushed = 0; }

    // One output sample came from buffer position `pos` (-1 = not from the loop, e.g. silence).
    void pushPosition(int64_t pos) noexcept
    {
        history[(size_t) (pushed & (kHistory - 1))] = pos;
        ++pushed;
    }

    // The buffer position that was playing `latency` samples before the i-th of the last `n`
    // pushed output samples -- or -1 if that's older than the history / before playback began.
    int64_t positionHeard(int i, int n, int latency) const noexcept
    {
        const int64_t g = pushed - n + i - latency;
        if (g < 0 || g < pushed - kHistory || g >= pushed)
            return -1;
        return history[(size_t) (g & (kHistory - 1))];
    }

    // Writes one block of input (`n` samples, matching the `n` positions just pushed for this
    // block's output) into `doc`. Level 0 at Feedback 1 is a bit-exact no-op.
    void write(juce::AudioBuffer<float>& doc, const juce::AudioBuffer<float>& input, int n,
               int latency, float level, float feedback) const noexcept
    {
        if (level == 0.0f && feedback == 1.0f)
            return;
        const int64_t docLen = doc.getNumSamples();
        const int docCh = doc.getNumChannels(), inCh = input.getNumChannels();
        if (docLen <= 0 || docCh <= 0 || inCh <= 0)
            return;
        for (int i = 0; i < juce::jmin(n, input.getNumSamples()); ++i)
        {
            const int64_t p = positionHeard(i, n, juce::jmax(0, latency));
            if (p < 0 || p >= docLen)
                continue;
            for (int ch = 0; ch < docCh; ++ch)
            {
                float* d = doc.getWritePointer(ch);
                const float x = input.getSample(juce::jmin(ch, inCh - 1), i);
                d[p] = softCeiling(d[p] * feedback + x * level);
            }
        }
    }

    // Untouched up to +-1, then a soft knee into +-1.5 -- stacked full-level layers saturate like
    // tape instead of running away (or turning into hard digital clipping).
    static float softCeiling(float y) noexcept
    {
        const float a = std::abs(y);
        if (a <= 1.0f) return y;
        const float out = 1.0f + 0.5f * std::tanh((a - 1.0f) / 0.5f);
        return y < 0.0f ? -out : out;
    }

private:
    std::vector<int64_t> history;
    int64_t pushed = 0;
};
}
