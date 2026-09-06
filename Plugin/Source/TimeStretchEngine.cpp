#include "TimeStretchEngine.h"
#include <rubberband/RubberBandStretcher.h>

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

    juce::AudioBuffer<float> output(numCh, 0);
    std::vector<float*> outPtrs((size_t) numCh);

    auto pullAvailable = [&]
    {
        int avail = stretcher.available();
        while (avail > 0)
        {
            int startSample = output.getNumSamples();
            output.setSize(numCh, startSample + avail, true, true, true);
            for (int ch = 0; ch < numCh; ++ch)
                outPtrs[(size_t) ch] = output.getWritePointer(ch) + startSample;
            size_t got = stretcher.retrieve(outPtrs.data(), (size_t) avail);
            if ((int) got < avail)
                output.setSize(numCh, startSample + (int) got, true, true, true);
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
    return output;
}

} // namespace TimeStretchEngine
