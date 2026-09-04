#include "EditActions.h"

namespace
{
    juce::AudioBuffer<float> extractRange(const juce::AudioBuffer<float>& src, int64_t start, int64_t end)
    {
        start = juce::jlimit((int64_t) 0, (int64_t) src.getNumSamples(), start);
        end   = juce::jlimit((int64_t) 0, (int64_t) src.getNumSamples(), end);
        int len = (int) juce::jmax((int64_t) 0, end - start);
        juce::AudioBuffer<float> out(src.getNumChannels(), len);
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
            out.copyFrom(ch, 0, src, ch, (int) start, len);
        return out;
    }

    // Builds a new buffer equal to: [0,start) + middle + [end,originalLength)
    juce::AudioBuffer<float> spliceReplace(const juce::AudioBuffer<float>& src, int64_t start, int64_t end,
                                            const juce::AudioBuffer<float>& middle)
    {
        const int numCh = src.getNumChannels();
        start = juce::jlimit((int64_t) 0, (int64_t) src.getNumSamples(), start);
        end   = juce::jlimit((int64_t) 0, (int64_t) src.getNumSamples(), end);

        const int headLen = (int) start;
        const int tailLen = src.getNumSamples() - (int) end;
        const int midLen  = middle.getNumSamples();
        const int totalLen = headLen + midLen + tailLen;

        juce::AudioBuffer<float> out(numCh, totalLen);
        for (int ch = 0; ch < numCh; ++ch)
        {
            int destPos = 0;
            if (headLen > 0) { out.copyFrom(ch, destPos, src, ch, 0, headLen); destPos += headLen; }
            if (midLen > 0)
            {
                int srcCh = juce::jmin(ch, juce::jmax(0, middle.getNumChannels() - 1));
                out.copyFrom(ch, destPos, middle, srcCh, 0, midLen);
                destPos += midLen;
            }
            if (tailLen > 0) out.copyFrom(ch, destPos, src, ch, (int) end, tailLen);
        }
        return out;
    }
}

namespace EditActions
{

void cut(AudioDocument& doc, Clipboard& clipboard)
{
    copy(doc, clipboard);
    deleteSelection(doc);
}

void copy(const AudioDocument& doc, Clipboard& clipboard)
{
    auto range = doc.getEffectiveRange();
    clipboard.buffer = extractRange(doc.getBuffer(), range.getStart(), range.getEnd());
    clipboard.sampleRate = doc.getSampleRate();
}

void pasteReplace(AudioDocument& doc, const Clipboard& clipboard)
{
    if (! clipboard.hasContent())
        return;

    auto range = doc.hasSelection() ? doc.getEffectiveRange()
                                     : juce::Range<int64_t>(doc.playhead.load(), doc.playhead.load());
    replaceRangeWith(doc, range, clipboard.buffer, "Paste");
}

void pasteInsert(AudioDocument& doc, const Clipboard& clipboard, int64_t atSample)
{
    if (! clipboard.hasContent())
        return;
    replaceRangeWith(doc, { atSample, atSample }, clipboard.buffer, "Paste");
}

void deleteSelection(AudioDocument& doc)
{
    if (! doc.hasSelection())
        return;
    auto range = doc.getEffectiveRange();
    juce::AudioBuffer<float> empty(juce::jmax(1, doc.getNumChannels()), 0);
    replaceRangeWith(doc, range, empty, "Delete");
}

void trimToSelection(AudioDocument& doc)
{
    if (! doc.hasSelection())
        return;
    auto range = doc.getEffectiveRange();

    doc.beginChange();
    auto kept = extractRange(doc.getBuffer(), range.getStart(), range.getEnd());
    doc.setSelection(0, 0);
    doc.commitChange(std::move(kept), "Trim");
}

void insertSilence(AudioDocument& doc, int64_t atSample, int64_t numSamples)
{
    if (numSamples <= 0)
        return;
    juce::AudioBuffer<float> silenceBuf(juce::jmax(1, doc.getNumChannels()), (int) numSamples);
    silenceBuf.clear();
    replaceRangeWith(doc, { atSample, atSample }, silenceBuf, "Insert Silence");
}

void normalize(AudioDocument& doc, float targetPeakDb)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;

    doc.beginChange();
    auto working = doc.getBuffer();
    float peak = 0.0f;
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
        peak = juce::jmax(peak, working.getMagnitude(ch, (int) range.getStart(), (int) range.getLength()));

    if (peak > 0.0001f)
    {
        float targetLinear = juce::Decibels::decibelsToGain(targetPeakDb);
        float gain = targetLinear / peak;
        for (int ch = 0; ch < working.getNumChannels(); ++ch)
            working.applyGain(ch, (int) range.getStart(), (int) range.getLength(), gain);
    }
    doc.commitChange(std::move(working), "Normalize");
}

void applyGainDb(AudioDocument& doc, float gainDb)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    float gain = juce::Decibels::decibelsToGain(gainDb);
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
        working.applyGain(ch, (int) range.getStart(), (int) range.getLength(), gain);
    doc.commitChange(std::move(working), "Gain");
}

void fadeIn(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
        working.applyGainRamp(ch, (int) range.getStart(), (int) range.getLength(), 0.0f, 1.0f);
    doc.commitChange(std::move(working), "Fade In");
}

void fadeOut(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
        working.applyGainRamp(ch, (int) range.getStart(), (int) range.getLength(), 1.0f, 0.0f);
    doc.commitChange(std::move(working), "Fade Out");
}

void reverse(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    int start = (int) range.getStart();
    int len = (int) range.getLength();
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
    {
        auto* d = working.getWritePointer(ch);
        std::reverse(d + start, d + start + len);
    }
    doc.commitChange(std::move(working), "Reverse");
}

void silence(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    for (int ch = 0; ch < working.getNumChannels(); ++ch)
        working.clear(ch, (int) range.getStart(), (int) range.getLength());
    doc.commitChange(std::move(working), "Silence");
}

void replaceRangeWith(AudioDocument& doc, juce::Range<int64_t> range, const juce::AudioBuffer<float>& newRegion,
                       const juce::String& actionName)
{
    doc.beginChange();
    auto spliced = spliceReplace(doc.getBuffer(), range.getStart(), range.getEnd(), newRegion);
    int64_t newCursor = range.getStart() + newRegion.getNumSamples();
    doc.setSelection(newCursor, newCursor);
    doc.playhead = juce::jlimit((int64_t) 0, (int64_t) spliced.getNumSamples(), newCursor);
    doc.commitChange(std::move(spliced), actionName);
}

bool exportChopSlices(const AudioDocument& doc, const juce::File& destFolder, const juce::String& baseName)
{
    if (doc.getNumSamples() <= 0)
        return false;

    std::vector<int64_t> bounds = doc.chopMarkers;
    if (bounds.empty() || bounds.front() != 0)
        bounds.insert(bounds.begin(), 0);
    bounds.push_back(doc.getNumSamples());

    juce::WavAudioFormat wavFormat;
    int index = 1;
    for (size_t i = 0; i + 1 < bounds.size(); ++i)
    {
        auto slice = extractRange(doc.getBuffer(), bounds[i], bounds[i + 1]);
        if (slice.getNumSamples() <= 0)
            continue;

        auto file = destFolder.getChildFile(baseName + "_" + juce::String(index++).paddedLeft('0', 3) + ".wav");
        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            continue;
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(), doc.getSampleRate(), (unsigned int) slice.getNumChannels(), 24, {}, 0));
        if (writer == nullptr)
            continue;
        stream.release();
        writer->writeFromAudioSampleBuffer(slice, 0, slice.getNumSamples());
    }
    return true;
}

} // namespace EditActions
