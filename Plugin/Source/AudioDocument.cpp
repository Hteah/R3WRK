#include "AudioDocument.h"
#include "TimeStretchEngine.h"
#include <algorithm>
#include <cmath>

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
    // A fresh document starts clean -- the Speed/Pitch/Stretch knobs and any Amplify/Stretch
    // preview are properties of the session with the *previous* audio, not this new one.
    // (Previously left as-is, so a KnobRow Stretch/Speed/Pitch set on one file silently
    // carried over -- visually and audibly -- into the next file loaded into the same
    // instance.)
    playbackSpeed = 1.0; playbackPitch = 0.0; playbackStretch = 1.0;
    previewActive = false; previewGainLinear = 1.0f; previewStretchRatio = 1.0;
    sliceMarkers.clear();
    undoManager.clearUndoHistory();
    markAsOriginal();
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
    playbackSpeed = 1.0; playbackPitch = 0.0; playbackStretch = 1.0;   // see newEmptyDocument()
    previewActive = false; previewGainLinear = 1.0f; previewStretchRatio = 1.0;
    sliceMarkers.clear();
    undoManager.clearUndoHistory();
    markAsOriginal();
    ++bufferVersion;
    notifyChanged();
    return true;
}

bool AudioDocument::playbackKnobsEngaged() const
{
    const double speed   = playbackSpeed.load(std::memory_order_relaxed);
    const double pitch   = playbackPitch.load(std::memory_order_relaxed);
    const double stretch = playbackStretch.load(std::memory_order_relaxed);
    return std::abs(speed - 1.0) > 1.0e-4
        || std::abs(pitch)       > 1.0e-4
        || std::abs(stretch - 1.0) > 1.0e-4;
}

juce::AudioBuffer<float> AudioDocument::renderWithPlaybackKnobs(const juce::AudioBuffer<float>& src) const
{
    juce::AudioBuffer<float> out;

    if (src.getNumSamples() <= 0 || src.getNumChannels() <= 0 || ! playbackKnobsEngaged())
    {
        out.makeCopyOf(src);
        return out;
    }

    const double speed   = juce::jlimit(kMinSpeed,   kMaxSpeed,   playbackSpeed.load(std::memory_order_relaxed));
    const double pitch   = juce::jlimit(kMinPitch,   kMaxPitch,   playbackPitch.load(std::memory_order_relaxed));
    const double stretch = juce::jlimit(kMinStretch, kMaxStretch, playbackStretch.load(std::memory_order_relaxed));

    // Same mapping as PluginProcessor::renderPlaybackStretched -- tape speed compresses time
    // and lifts pitch, the Pitch knob layers on extra semitones, Stretch dilates time only.
    const double timeRatio = stretch / juce::jmax(1.0e-4, speed);
    const double semitones = 12.0 * std::log2(juce::jmax(1.0e-4, speed)) + pitch;

    out = TimeStretchEngine::process(src, sampleRate, timeRatio, semitones);
    if (out.getNumSamples() <= 0)          // engine failure -> fall back to the dry audio
        out.makeCopyOf(src);
    return out;
}

bool AudioDocument::saveToFile(const juce::File& file) const
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    // Bake the Speed/Pitch/Stretch knobs into the written audio (a no-op copy when they're
    // centred), so the file is what you hear rather than the dry clip + separate knob state.
    const juce::AudioBuffer<float> rendered = renderWithPlaybackKnobs(buffer);

    juce::WavAudioFormat wavFormat;
    auto writer = wavFormat.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                        .withSampleRate(sampleRate)
                                                        .withNumChannels(rendered.getNumChannels())
                                                        .withBitsPerSample(24));
    if (writer == nullptr)
        return false;   // stream is left intact on failure; unique_ptr cleans it up

    writer->writeFromAudioSampleBuffer(rendered, 0, rendered.getNumSamples());
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

    // One commitChange() = one undo step. juce::UndoManager otherwise keeps appending every
    // perform() to whatever transaction is already open -- it only starts a fresh one right
    // after construction or a clearUndoHistory() call, never automatically past that point --
    // so without this, every edit made since the last load/record/clear (Trim, then Amplify,
    // then Fade In, ...) would silently pile into a *single* undo step, and one Undo would
    // revert all of them at once instead of just the last one.
    undoManager.beginNewTransaction(actionName);
    undoManager.perform(action.release());
    notifyChanged();
}

void AudioDocument::markAsOriginal()
{
    originalBuffer.makeCopyOf(buffer);
}

void AudioDocument::revertToOriginal()
{
    if (isEmpty())
        return;
    beginChange();
    commitChange(originalBuffer, "Revert to Original");
}

void AudioDocument::restoreSnapshot(const juce::AudioBuffer<float>& newBuffer, int64_t newSelStart, int64_t newSelEnd)
{
    const int64_t oldLen = getNumSamples();
    {
        const juce::ScopedLock sl(bufferLock);
        buffer.makeCopyOf(newBuffer);
    }
    selPacked.store(packSelection(newSelStart, newSelEnd), std::memory_order_relaxed);
    playhead = juce::jlimit((int64_t) 0, getNumSamples(), playhead.load());

    // Loop is always "the whole clip" (there's no UI for a partial loop region -- processBlock
    // uses the selection when there is one, else [0, numSamples]). Pin it back to the full
    // buffer, not just clamp it: otherwise a Trim shrinks loopEnd, and the Undo that restores
    // the long buffer leaves loopEnd stranded mid-clip -- a phantom loop marker that also caps
    // playback there.
    loopStart = 0;
    loopEnd = getNumSamples();

    // A length-changing edit (trim/cut/paste/stretch/...) invalidates every slice marker's
    // absolute position, so drop them rather than leave them silently pointing at the wrong
    // audio. Edits that keep the length (amplify, normalize, fade, reverse, silence) keep
    // them. Not restored by undo -- markers aren't in the snapshot.
    if (getNumSamples() != oldLen)
        sliceMarkers.clear();

    ++bufferVersion;
    notifyChanged();
}

void AudioDocument::normaliseSliceMarkers()
{
    const int64_t n = getNumSamples();
    for (auto& m : sliceMarkers)
        m = juce::jlimit((int64_t) 0, n, m);

    std::sort(sliceMarkers.begin(), sliceMarkers.end());
    sliceMarkers.erase(std::unique(sliceMarkers.begin(), sliceMarkers.end()), sliceMarkers.end());

    // markers at 0 or at the very end aren't real cut points (they'd make an empty region).
    sliceMarkers.erase(std::remove_if(sliceMarkers.begin(), sliceMarkers.end(),
                                      [n](int64_t m) { return m <= 0 || m >= n; }),
                       sliceMarkers.end());
}

void AudioDocument::addSliceMarker(int64_t sample)
{
    sliceMarkers.push_back(sample);
    normaliseSliceMarkers();
    notifyChanged();
}

void AudioDocument::removeSliceMarker(int index)
{
    if (index >= 0 && index < (int) sliceMarkers.size())
    {
        sliceMarkers.erase(sliceMarkers.begin() + index);
        notifyChanged();
    }
}

int AudioDocument::moveSliceMarker(int index, int64_t newSample)
{
    if (index < 0 || index >= (int) sliceMarkers.size())
        return -1;

    newSample = juce::jlimit((int64_t) 1, juce::jmax((int64_t) 1, getNumSamples() - 1), newSample);
    sliceMarkers[(size_t) index] = newSample;
    normaliseSliceMarkers();   // re-sorts; if dragged exactly onto another marker the two collapse to one
    notifyChanged();

    // `newSample` is always in range, so a marker sits there after normalising -- return its index
    // (the same one, or the surviving one if a merge happened) so a drag can keep tracking it.
    for (int i = 0; i < (int) sliceMarkers.size(); ++i)
        if (sliceMarkers[(size_t) i] == newSample)
            return i;
    return -1;   // shouldn't happen
}

void AudioDocument::clearSliceMarkers()
{
    if (! sliceMarkers.empty())
    {
        sliceMarkers.clear();
        notifyChanged();
    }
}

int AudioDocument::findSliceMarkerNear(int64_t sample, int64_t tolerance) const
{
    int best = -1;
    int64_t bestDist = tolerance + 1;
    for (int i = 0; i < (int) sliceMarkers.size(); ++i)
    {
        const int64_t diff = sliceMarkers[(size_t) i] - sample;
        const int64_t d = diff < 0 ? -diff : diff;
        if (d <= tolerance && d < bestDist)
        {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

std::vector<juce::Range<int64_t>> AudioDocument::getSliceRegions() const
{
    std::vector<juce::Range<int64_t>> regions;
    if (sliceMarkers.empty())
        return regions;

    const int64_t n = getNumSamples();
    int64_t prev = 0;
    for (int64_t m : sliceMarkers)   // already sorted/unique/in-range
    {
        regions.push_back({ prev, m });
        prev = m;
    }
    regions.push_back({ prev, n });
    return regions;
}
