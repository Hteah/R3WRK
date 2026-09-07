#include "EditActions.h"
#include "OctatrackOtFile.h"

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

    // Writes `region` to `file` as a 24-bit WAV, overwriting anything already there.
    bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& region, double sampleRate)
    {
        if (region.getNumSamples() <= 0)
            return false;

        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wavFormat;
        auto writer = wavFormat.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                            .withSampleRate(sampleRate)
                                                            .withNumChannels(region.getNumChannels())
                                                            .withBitsPerSample(24));
        if (writer == nullptr)
            return false;

        writer->writeFromAudioSampleBuffer(region, 0, region.getNumSamples());
        return true;
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

bool exportSelection(const AudioDocument& doc, const juce::File& file)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return false;

    // Bake the Speed/Pitch/Stretch knobs into the exported region (no-op copy when centred),
    // same as Save As -- the file is the sound, not the knob positions.
    auto region = extractRange(doc.getBuffer(), range.getStart(), range.getEnd());
    return writeWav(file, doc.renderWithPlaybackKnobs(region), doc.getSampleRate());
}

int sliceToFolder(const AudioDocument& doc, const juce::File& folder, const juce::String& baseName)
{
    const auto regions = doc.getSliceRegions();
    if (regions.empty())
        return 0;

    folder.createDirectory();
    const auto name = baseName.isNotEmpty() ? baseName : juce::String("slice");

    int written = 0;
    for (int i = 0; i < (int) regions.size(); ++i)
    {
        const auto file = folder.getChildFile(name + " " + juce::String(i + 1).paddedLeft('0', 2) + ".wav");
        if (writeWav(file, extractRange(doc.getBuffer(), regions[(size_t) i].getStart(), regions[(size_t) i].getEnd()),
                     doc.getSampleRate()))
            ++written;
    }
    return written;
}

bool exportOctatrackChain(const AudioDocument& doc, const juce::File& wavFile, double bpm)
{
    const int64_t len = doc.getNumSamples();
    if (len <= 0)
        return false;

    auto regions = doc.getSliceRegions();
    if (regions.empty())
        regions.push_back({ (int64_t) 0, len });          // no markers -> one whole-clip slice
    if ((int) regions.size() > OctatrackOtFile::kMaxSlices)
        regions.resize(OctatrackOtFile::kMaxSlices);        // Octatrack's hard limit

    if (! writeWav(wavFile, extractRange(doc.getBuffer(), 0, len), doc.getSampleRate()))
        return false;

    return OctatrackOtFile::writeToFile(wavFile.withFileExtension("ot"), len, regions, bpm);
}

} // namespace EditActions
