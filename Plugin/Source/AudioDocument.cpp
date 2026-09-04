#include "AudioDocument.h"

namespace
{
    class SnapshotAction : public juce::UndoableAction
    {
    public:
        SnapshotAction(AudioDocument& doc,
                        juce::AudioBuffer<float> before, int64_t beforeSelS, int64_t beforeSelE,
                        juce::AudioBuffer<float> after,  int64_t afterSelS,  int64_t afterSelE)
            : document(doc),
              beforeBuffer(std::move(before)), beforeSelStart(beforeSelS), beforeSelEnd(beforeSelE),
              afterBuffer(std::move(after)),   afterSelStart(afterSelS),   afterSelEnd(afterSelE)
        {
        }

        bool perform() override
        {
            document.restoreSnapshot(afterBuffer, afterSelStart, afterSelEnd);
            return true;
        }

        bool undo() override
        {
            document.restoreSnapshot(beforeBuffer, beforeSelStart, beforeSelEnd);
            return true;
        }

        int getSizeInUnits() override
        {
            return (int) ((size_t) beforeBuffer.getNumSamples() * sizeof(float) * (size_t) juce::jmax(1, beforeBuffer.getNumChannels())
                         + (size_t) afterBuffer.getNumSamples()  * sizeof(float) * (size_t) juce::jmax(1, afterBuffer.getNumChannels()));
        }

    private:
        AudioDocument& document;
        juce::AudioBuffer<float> beforeBuffer;
        int64_t beforeSelStart, beforeSelEnd;
        juce::AudioBuffer<float> afterBuffer;
        int64_t afterSelStart, afterSelEnd;
    };
}

AudioDocument::AudioDocument()
{
    buffer.setSize(2, 0);
}

void AudioDocument::newEmptyDocument(int numChannels, double sr)
{
    {
        const juce::ScopedLock sl(bufferLock);
        buffer.setSize(numChannels, 0);
    }
    sampleRate = sr;
    selStart = selEnd = 0;
    playhead = 0;
    loopStart = loopEnd = 0;
    loopEnabled = false;
    chopMarkers.clear();
    undoManager.clearUndoHistory();
    ++bufferVersion;
    notifyChanged();
}

bool AudioDocument::loadFromFile(const juce::File& file, double resampleToRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr)
        return false;

    juce::AudioBuffer<float> newBuffer((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read(&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    double finalRate = reader->sampleRate;

    if (resampleToRate > 0.0 && std::abs(resampleToRate - finalRate) > 0.5 && newBuffer.getNumSamples() > 0)
    {
        double ratio = finalRate / resampleToRate;
        int newLength = (int) std::ceil((double) newBuffer.getNumSamples() / ratio) + 1;
        juce::AudioBuffer<float> resampled(newBuffer.getNumChannels(), newLength);
        for (int ch = 0; ch < newBuffer.getNumChannels(); ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.reset();
            int produced = interpolator.process(ratio, newBuffer.getReadPointer(ch), resampled.getWritePointer(ch),
                                                 newLength);
            juce::ignoreUnused(produced);
        }
        newBuffer = std::move(resampled);
        finalRate = resampleToRate;
    }

    {
        const juce::ScopedLock sl(bufferLock);
        buffer = std::move(newBuffer);
    }
    sampleRate = finalRate;
    selStart = selEnd = 0;
    playhead = 0;
    loopStart = 0;
    loopEnd = getNumSamples();
    loopEnabled = false;
    chopMarkers.clear();
    undoManager.clearUndoHistory();
    ++bufferVersion;
    notifyChanged();
    return true;
}

bool AudioDocument::saveToFile(const juce::File& file) const
{
    juce::WavAudioFormat wavFormat;
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(stream.get(), sampleRate, (unsigned int) buffer.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return false;

    stream.release(); // writer now owns the stream
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    return true;
}

void AudioDocument::setSelection(int64_t start, int64_t end)
{
    auto n = getNumSamples();
    start = juce::jlimit((int64_t) 0, n, start);
    end   = juce::jlimit((int64_t) 0, n, end);
    if (end < start)
        std::swap(start, end);
    selStart = start;
    selEnd = end;
    notifyChanged();
}

juce::Range<int64_t> AudioDocument::getEffectiveRange() const
{
    if (hasSelection())
        return { selStart, selEnd };
    return { (int64_t) 0, getNumSamples() };
}

void AudioDocument::recalculateChopMarkers()
{
    chopMarkers.clear();
    if (chopBpm <= 0.0 || chopDivision <= 0 || getNumSamples() <= 0)
        return;

    const double secondsPerBeat = 60.0 / chopBpm;
    const double secondsPerSlice = secondsPerBeat * (4.0 / (double) chopDivision);
    const double samplesPerSlice = secondsPerSlice * sampleRate;
    if (samplesPerSlice < 1.0)
        return;

    for (double pos = 0.0; pos < (double) getNumSamples(); pos += samplesPerSlice)
        chopMarkers.push_back((int64_t) pos);
}

void AudioDocument::beginChange()
{
    jassert(! changeInProgress);
    preChangeBuffer.makeCopyOf(buffer);
    preChangeSelStart = selStart;
    preChangeSelEnd = selEnd;
    changeInProgress = true;
}

void AudioDocument::commitChange(juce::AudioBuffer<float> newBuffer, const juce::String& actionName)
{
    jassert(changeInProgress);
    changeInProgress = false;

    auto action = std::make_unique<SnapshotAction>(*this,
                                                     preChangeBuffer, preChangeSelStart, preChangeSelEnd,
                                                     std::move(newBuffer), selStart, selEnd);
    undoManager.perform(action.release(), actionName);
    notifyChanged();
}

void AudioDocument::restoreSnapshot(const juce::AudioBuffer<float>& newBuffer, int64_t newSelStart, int64_t newSelEnd)
{
    {
        const juce::ScopedLock sl(bufferLock);
        buffer.makeCopyOf(newBuffer);
    }
    selStart = newSelStart;
    selEnd = newSelEnd;
    playhead = juce::jlimit((int64_t) 0, getNumSamples(), playhead.load());
    loopStart = juce::jlimit((int64_t) 0, getNumSamples(), loopStart.load());
    loopEnd = juce::jlimit((int64_t) 0, getNumSamples(), loopEnd.load());
    recalculateChopMarkers();
    ++bufferVersion;
    notifyChanged();
}
