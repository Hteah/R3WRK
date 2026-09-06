#include "TimeStretchEngine.h"
#include <rubberband/RubberBandStretcher.h>
#include <climits>

namespace TimeStretchEngine
{

juce::AudioBuffer<float> process(const juce::AudioBuffer<float>& input, double sampleRate,
                                  double stretchRatio, double pitchSemitones, bool fastPreview)
{
    using namespace RubberBand;

    const int numCh = input.getNumChannels();
    const int numFrames = input.getNumSamples();
    if (numCh <= 0 || numFrames <= 0)
        return {};

    auto options = fastPreview
        ? (RubberBandStretcher::OptionProcessOffline
           | RubberBandStretcher::OptionEngineFaster
           | RubberBandStretcher::OptionWindowShort
           | RubberBandStretcher::OptionPitchHighSpeed)
        : (RubberBandStretcher::OptionProcessOffline
           | RubberBandStretcher::OptionEngineFiner
           | RubberBandStretcher::OptionPitchHighQuality);

    RubberBandStretcher stretcher((size_t) sampleRate, (size_t) numCh, options);
    stretcher.setTimeRatio(juce::jmax(0.01, stretchRatio));
    stretcher.setPitchScale(std::pow(2.0, pitchSemitones / 12.0));
    stretcher.setExpectedInputDuration((size_t) numFrames);

    std::vector<const float*> inPtrs((size_t) numCh);
    for (int ch = 0; ch < numCh; ++ch)
        inPtrs[(size_t) ch] = input.getReadPointer(ch);

    const int blockSize = 4096;

    // Pre-size to the expected output length. The old code grew `output` by exactly `avail`
    // samples on every retrieve, with keepExistingContent=true -- i.e. reallocating and copying
    // the whole buffer-so-far each time. At a 20x ratio that's an accidental O(n^2): millions
    // of output samples in ~4k chunks -> billions of sample-copies -> minutes. Size it once up
    // front (+ slack for RubberBand's rounding and end tail), track a write cursor, and only
    // double capacity in the rare case the estimate falls short.
    const int64_t estOut = (int64_t) ((double) numFrames * juce::jmax(0.01, stretchRatio) * 1.15)
                         + blockSize * 4;
    juce::AudioBuffer<float> output(numCh, (int) juce::jlimit((int64_t) blockSize, (int64_t) INT_MAX, estOut));
    int writePos = 0;
    std::vector<float*> outPtrs((size_t) numCh);

    auto pullAvailable = [&]
    {
        int avail = stretcher.available();
        while (avail > 0)
        {
            if (writePos + avail > output.getNumSamples())
                output.setSize(numCh, (writePos + avail) * 2, /*keep*/ true, false, /*avoidRealloc*/ true);
            for (int ch = 0; ch < numCh; ++ch)
                outPtrs[(size_t) ch] = output.getWritePointer(ch) + writePos;
            size_t got = stretcher.retrieve(outPtrs.data(), (size_t) avail);
            writePos += (int) got;
            if ((int) got < avail)
                break;
            avail = stretcher.available();
        }
    };

    // Study pass: RubberBand needs a look-ahead analysis pass before processing in offline mode.
    {
        int pos = 0;
        while (pos < numFrames)
        {
            int thisBlock = juce::jmin(blockSize, numFrames - pos);
            std::vector<const float*> blockPtrs((size_t) numCh);
            for (int ch = 0; ch < numCh; ++ch)
                blockPtrs[(size_t) ch] = inPtrs[(size_t) ch] + pos;
            bool isFinal = (pos + thisBlock) >= numFrames;
            stretcher.study(blockPtrs.data(), (size_t) thisBlock, isFinal);
            pos += thisBlock;
        }
    }

    // Process pass
    {
        int pos = 0;
        while (pos < numFrames)
        {
            int thisBlock = juce::jmin(blockSize, numFrames - pos);
            std::vector<const float*> blockPtrs((size_t) numCh);
            for (int ch = 0; ch < numCh; ++ch)
                blockPtrs[(size_t) ch] = inPtrs[(size_t) ch] + pos;
            bool isFinal = (pos + thisBlock) >= numFrames;
            stretcher.process(blockPtrs.data(), (size_t) thisBlock, isFinal);
            pos += thisBlock;
            pullAvailable();
        }
    }

    pullAvailable();
    output.setSize(numCh, writePos, /*keep*/ true, false, /*avoidRealloc*/ true);   // trim to actual
    return output;
}

} // namespace TimeStretchEngine
