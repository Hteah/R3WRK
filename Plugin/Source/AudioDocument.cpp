#include "AudioDocument.h"

namespace
{
    uint64_t packSelection(int64_t start, int64_t end)
    {
        return ((uint64_t) (uint32_t) start << 32) | (uint64_t) (uint32_t) end;
    }

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
            const int64_t samples = (int64_t) beforeBuffer.getNumSamples() * juce::jmax(1, beforeBuffer.getNumChannels())
                                  + (int64_t) afterBuffer.getNumSamples()  * juce::jmax(1, afterBuffer.getNumChannels());
            return (int) juce::jmin((int64_t) 0x7fffffff, samples * (int64_t) sizeof(float));
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

    // Each undo step is a full before+after copy of the audio (snapshot model), so cap the
    // history by memory rather than letting it grow without bound on a long clip.
    undoManager.setMaxNumberOfStoredUnits(128 * 1024 * 1024, 24);
}

void AudioDocument::newEmptyDocument(int numChannels, double sr)
{
    {
        const juce::ScopedLock sl(bufferLock);
        buffer.setSize(numChannels, 0);
    }
    sampleRate = sr;
    selPacked.store(0, std::memory_order_relaxed);
    playhead = 0;
    loopStart = loopEnd = 0;
    loopEnabled = false;
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
    selPacked.store(0, std::memory_order_relaxed);
    playhead = 0;
    loopStart = 0;
    loopEnd = getNumSamples();
    loopEnabled = false;
    undoManager.clearUndoHistory();
    ++bufferVersion;
    notifyChanged();
    return true;
}

bool AudioDocument::saveToFile(const juce::File& file) const
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wavFormat;
    auto writer = wavFormat.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                        .withSampleRate(sampleRate)
                                                        .withNumChannels(buffer.getNumChannels())
                                                        .withBitsPerSample(24));
    if (writer == nullptr)
        return false;   // stream is left intact on failure; unique_ptr cleans it up

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
    selPacked.store(packSelection(start, end), std::memory_order_relaxed);
    notifyChanged();
}

juce::Range<int64_t> AudioDocument::getSelection() const
{
    const uint64_t p = selPacked.load(std::memory_order_relaxed);
    return { (int64_t) (int32_t) (p >> 32), (int64_t) (int32_t) (p & 0xffffffffu) };
}

juce::Range<int64_t> AudioDocument::getEffectiveRange() const
{
    if (hasSelection())
        return getSelection();
    return { (int64_t) 0, getNumSamples() };
}

void AudioDocument::resetRecordingScope()
{
    std::fill(scopeMin, scopeMin + scopeSize, 0.0f);
    std::fill(scopeMax, scopeMax + scopeSize, 0.0f);
    scopeWritePos.store(0, std::memory_order_release);
    recordedSamples.store(0, std::memory_order_relaxed);
}

void AudioDocument::beginChange()
{
    jassert(! changeInProgress);
    preChangeBuffer.makeCopyOf(buffer);
    preChangeSelStart = getSelectionStart();
    preChangeSelEnd = getSelectionEnd();
    changeInProgress = true;
}

void AudioDocument::commitChange(juce::AudioBuffer<float> newBuffer, const juce::String& actionName)
{
    jassert(changeInProgress);
    changeInProgress = false;

    auto action = std::make_unique<SnapshotAction>(*this,
                                                     preChangeBuffer, preChangeSelStart, preChangeSelEnd,
                                                     std::move(newBuffer), getSelectionStart(), getSelectionEnd());
    undoManager.perform(action.release(), actionName);
    notifyChanged();
}

void AudioDocument::restoreSnapshot(const juce::AudioBuffer<float>& newBuffer, int64_t newSelStart, int64_t newSelEnd)
{
    {
        const juce::ScopedLock sl(bufferLock);
        buffer.makeCopyOf(newBuffer);
    }
    selPacked.store(packSelection(newSelStart, newSelEnd), std::memory_order_relaxed);
    playhead = juce::jlimit((int64_t) 0, getNumSamples(), playhead.load());
    loopStart = juce::jlimit((int64_t) 0, getNumSamples(), loopStart.load());
    loopEnd = juce::jlimit((int64_t) 0, getNumSamples(), loopEnd.load());
    ++bufferVersion;
    notifyChanged();
}
