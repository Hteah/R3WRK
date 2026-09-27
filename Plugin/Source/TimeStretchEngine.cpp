#include "TimeStretchEngine.h"
#include "LofiStretch.h"
#include <climits>

namespace TimeStretchEngine
{

juce::AudioBuffer<float> process(const juce::AudioBuffer<float>& input, double sampleRate,
                                 double speed, double stretch, double pitchSemitones)
{
    const int numCh = input.getNumChannels();
    const int numFrames = input.getNumSamples();
    if (numCh <= 0 || numFrames <= 0 || sampleRate <= 0.0)
        return {};

    speed = juce::jmax(0.01, speed);
    stretch = juce::jmax(0.01, stretch);
    const int64_t expected = (int64_t) std::llround((double) numFrames * stretch / speed);
    if (expected <= 0 || expected > INT_MAX)
        return {};

    juce::AudioBuffer<float> output(numCh, (int) expected);
    output.clear();

    // The engine handles up to stereo; wider buffers go through in pairs.
    for (int first = 0; first < numCh; first += r3wrk::LofiStretch::kMaxChannels)
    {
        const int n = juce::jmin(r3wrk::LofiStretch::kMaxChannels, numCh - first);
        r3wrk::LofiStretch eng;
        eng.prepare(sampleRate, n);
        eng.setParams(speed, stretch, pitchSemitones);
        eng.reset();
        const int latency = eng.latency();

        juce::AudioBuffer<float> chunkOut(n, 8192);
        int64_t produced = 0;   // engine output samples seen, including the latency lead-in
        auto drain = [&]
        {
            for (;;)
            {
                const int got = eng.retrieve(chunkOut.getArrayOfWritePointers(), chunkOut.getNumSamples());
                if (got <= 0)
                    break;
                for (int i = 0; i < got; ++i)
                {
                    const int64_t o = produced + i - latency;
                    if (o >= 0 && o < expected)
                        for (int c = 0; c < n; ++c)
                            output.setSample(first + c, (int) o, chunkOut.getSample(c, i));
                }
                produced += got;
            }
        };

        const float* ptrs[r3wrk::LofiStretch::kMaxChannels];
        int pos = 0;
        while (pos < numFrames)
        {
            const int block = juce::jmin(4096, numFrames - pos);
            for (int c = 0; c < n; ++c)
                ptrs[c] = input.getReadPointer(first + c) + pos;
            eng.process(ptrs, block, false);
            pos += block;
            drain();
        }
        eng.process(nullptr, 0, true);
        drain();
    }
    return output;
}

} // namespace TimeStretchEngine
