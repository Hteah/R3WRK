#include "SpectrogramDisplay.h"

namespace
{
    juce::Colour spectrumColour(float norm)
    {
        norm = juce::jlimit(0.0f, 1.0f, norm);
        // dark -> blue -> magenta -> orange -> yellow, brightness rises with norm
        return juce::Colour::fromHSV(0.72f - 0.72f * norm, 0.85f, std::pow(norm, 0.6f), 1.0f);
    }
}

SpectrogramDisplay::SpectrogramDisplay(AudioDocument& doc) : document(doc)
{
    document.changeBroadcaster.addChangeListener(this);
}

SpectrogramDisplay::~SpectrogramDisplay()
{
    document.changeBroadcaster.removeChangeListener(this);
}

int64_t SpectrogramDisplay::xToSample(float x) const
{
    int64_t n = document.getNumSamples();
    return juce::jlimit((int64_t) 0, n, (int64_t) ((double) x / (double) juce::jmax(1, getWidth()) * (double) n));
}

float SpectrogramDisplay::sampleToX(int64_t sample) const
{
    int64_t n = juce::jmax((int64_t) 1, document.getNumSamples());
    return (float) getWidth() * (float) ((double) sample / (double) n);
}

void SpectrogramDisplay::rebuildImage()
{
    const int numSamples = (int) document.getNumSamples();
    if (numSamples <= 0)
    {
        spectrogramImage = {};
        return;
    }

    constexpr int fftOrder = 11;
    constexpr int fftSize = 1 << fftOrder; // 2048
    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann);

    const int maxColumns = 1024;
    const int hop = juce::jmax(fftSize / 4, numSamples / maxColumns);
    int numFrames = juce::jmax(1, (numSamples - fftSize) / hop + 1);
    numFrames = juce::jmin(numFrames, maxColumns);
    const int outHeight = 256;

    auto& buf = document.getBuffer();
    const int numCh = buf.getNumChannels();

    std::vector<float> mono((size_t) numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            sum += buf.getSample(ch, i);
        mono[(size_t) i] = sum / (float) juce::jmax(1, numCh);
    }

    juce::Image img(juce::Image::RGB, juce::jmax(1, numFrames), outHeight, true);
    std::vector<float> fftBuffer((size_t) fftSize * 2, 0.0f);
    const int numBins = fftSize / 2;

    for (int frame = 0; frame < numFrames; ++frame)
    {
        int start = frame * hop;
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        int available = juce::jmin(fftSize, numSamples - start);
        if (available > 0)
            std::copy(mono.begin() + start, mono.begin() + start + available, fftBuffer.begin());

        window.multiplyWithWindowingTable(fftBuffer.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform(fftBuffer.data());

        for (int y = 0; y < outHeight; ++y)
        {
            int binHi = numBins - (int) ((double) y / outHeight * numBins);
            int binLo = numBins - (int) ((double) (y + 1) / outHeight * numBins);
            binLo = juce::jmax(0, binLo);
            binHi = juce::jmax(binLo + 1, juce::jmin(numBins, binHi));

            float mag = 0.0f;
            for (int b = binLo; b < binHi; ++b)
                mag = juce::jmax(mag, fftBuffer[(size_t) b]);

            float db = juce::Decibels::gainToDecibels(mag, -100.0f);
            float norm = juce::jlimit(0.0f, 1.0f, (db + 100.0f) / 100.0f);
            img.setPixelAt(frame, y, spectrumColour(norm));
        }
    }

    spectrogramImage = img;
    lastSeenVersion = document.getBufferVersion();
}

void SpectrogramDisplay::resized()
{
    // image is resolution-independent when drawn scaled; nothing to rebuild on resize.
}

void SpectrogramDisplay::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (document.getBufferVersion() != lastSeenVersion)
        rebuildImage();
    repaint();
}

void SpectrogramDisplay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1e23));

    if (document.isEmpty())
    {
        g.setColour(juce::Colours::grey);
        g.drawText("No audio loaded", getLocalBounds(), juce::Justification::centred);
        return;
    }

    if (lastSeenVersion != document.getBufferVersion())
        rebuildImage();

    if (spectrogramImage.isValid())
        g.drawImage(spectrogramImage, getLocalBounds().toFloat());

    if (document.hasSelection())
    {
        float x0 = sampleToX(document.getSelectionStart());
        float x1 = sampleToX(document.getSelectionEnd());
        g.setColour(juce::Colours::white.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(x0, 0.0f, x1 - x0, (float) getHeight()));
    }

    g.setColour(juce::Colours::red);
    g.drawVerticalLine((int) sampleToX(document.playhead.load()), 0.0f, (float) getHeight());
}

void SpectrogramDisplay::mouseDown(const juce::MouseEvent& e)
{
    dragStartSample = xToSample((float) e.x);
    document.playhead = dragStartSample;
    document.setSelection(dragStartSample, dragStartSample);
}

void SpectrogramDisplay::mouseDrag(const juce::MouseEvent& e)
{
    document.setSelection(dragStartSample, xToSample((float) e.x));
}
