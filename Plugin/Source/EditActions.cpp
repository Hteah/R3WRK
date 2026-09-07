#include "EditActions.h"
#include "OctatrackOtFile.h"
#include <cmath>

namespace
{
    // Runs fn(ch) for each channel currently in editing focus (see AudioDocument::channelFocus).
    template <typename Fn>
    void forEachFocusedChannel(const AudioDocument& doc, int numChannels, Fn&& fn)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            if (doc.channelInFocus(ch))
                fn(ch);
    }

    // Maps contiguous raw-sample slice regions into a buffer of `renderedLen` samples by
    // scaling every boundary proportionally. After renderWithPlaybackKnobs() has dilated or
    // compressed time (Speed/Stretch), the cuts still land on the same audio; endpoints stay
    // exact (0 -> 0, rawLen -> renderedLen) so the regions still tile [0, renderedLen] with
    // no gaps. Returns the regions unchanged when nothing stretched.
    std::vector<juce::Range<int64_t>> scaleRegions(const std::vector<juce::Range<int64_t>>& regions,
                                                   int64_t rawLen, int64_t renderedLen)
    {
        if (regions.empty() || rawLen <= 0 || renderedLen <= 0 || rawLen == renderedLen)
            return regions;

        const double s = (double) renderedLen / (double) rawLen;
        auto mapPos = [&](int64_t p)
        {
            return juce::jlimit((int64_t) 0, renderedLen, (int64_t) std::llround((double) p * s));
        };

        std::vector<juce::Range<int64_t>> out;
        out.reserve(regions.size());
        for (const auto& r : regions)
            out.push_back({ mapPos(r.getStart()), mapPos(r.getEnd()) });
        return out;
    }

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

    // Writes `region` to `file` as a WAV (24-bit unless told otherwise), overwriting anything
    // already there.
    bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& region, double sampleRate,
                  int bitsPerSample = 24)
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
                                                            .withBitsPerSample(bitsPerSample));
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
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
        { peak = juce::jmax(peak, working.getMagnitude(ch, (int) range.getStart(), (int) range.getLength())); });

    if (peak > 0.0001f)
    {
        float targetLinear = juce::Decibels::decibelsToGain(targetPeakDb);
        float gain = targetLinear / peak;
        forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
            { working.applyGain(ch, (int) range.getStart(), (int) range.getLength(), gain); });
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
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
        { working.applyGain(ch, (int) range.getStart(), (int) range.getLength(), gain); });
    doc.commitChange(std::move(working), "Gain");
}

void fadeIn(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
        { working.applyGainRamp(ch, (int) range.getStart(), (int) range.getLength(), 0.0f, 1.0f); });
    doc.commitChange(std::move(working), "Fade In");
}

void fadeOut(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
        { working.applyGainRamp(ch, (int) range.getStart(), (int) range.getLength(), 1.0f, 0.0f); });
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
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
    {
        auto* d = working.getWritePointer(ch);
        std::reverse(d + start, d + start + len);
    });
    doc.commitChange(std::move(working), "Reverse");
}

void silence(AudioDocument& doc)
{
    auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return;
    doc.beginChange();
    auto working = doc.getBuffer();
    forEachFocusedChannel(doc, working.getNumChannels(), [&](int ch)
        { working.clear(ch, (int) range.getStart(), (int) range.getLength()); });
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
    const auto rawRegions = doc.getSliceRegions();
    if (rawRegions.empty())
        return 0;

    // Bake Speed/Pitch/Stretch into the audio (a no-op copy when the knobs are centred), then
    // slice the rendered buffer -- scaleRegions() moves each marker along with the stretch so
    // every slice still cuts the same musical moment it did on screen.
    const auto rendered = doc.renderWithPlaybackKnobs(doc.getBuffer());
    const auto regions  = scaleRegions(rawRegions, doc.getNumSamples(), rendered.getNumSamples());

    folder.createDirectory();
    const auto name = baseName.isNotEmpty() ? baseName : juce::String("slice");

    int written = 0;
    for (int i = 0; i < (int) regions.size(); ++i)
    {
        const auto file = folder.getChildFile(name + " " + juce::String(i + 1).paddedLeft('0', 2) + ".wav");
        if (writeWav(file, extractRange(rendered, regions[(size_t) i].getStart(), regions[(size_t) i].getEnd()),
                     doc.getSampleRate()))
            ++written;
    }
    return written;
}

bool exportOctatrackChain(const AudioDocument& doc, const juce::File& wavFile, double bpm)
{
    // The Octatrack only reads 16-bit / 44.1 kHz WAVs, so a chain is always written that way
    // regardless of the Save Options -- it's a device format, not a user choice.
    constexpr double kOtRate = 44100.0;
    constexpr int    kOtBits = 16;

    const int64_t rawLen = doc.getNumSamples();
    if (rawLen <= 0)
        return false;

    // Bake the knobs, force 44.1 kHz, then scale the slice points to the resampled length so
    // the .wav and the .ot slice grid agree.
    auto rendered = doc.renderWithPlaybackKnobs(doc.getBuffer());
    rendered = AudioDocument::resampled(rendered, doc.getSampleRate(), kOtRate);
    const int64_t len = rendered.getNumSamples();
    if (len <= 0)
        return false;

    auto rawRegions = doc.getSliceRegions();
    if (rawRegions.empty())
        rawRegions.push_back({ (int64_t) 0, rawLen });      // no markers -> one whole-clip slice

    auto regions = scaleRegions(rawRegions, rawLen, len);
    if ((int) regions.size() > OctatrackOtFile::kMaxSlices)
        regions.resize(OctatrackOtFile::kMaxSlices);        // Octatrack's hard limit

    if (! writeWav(wavFile, rendered, kOtRate, kOtBits))
        return false;

    return OctatrackOtFile::writeToFile(wavFile.withFileExtension("ot"), len, regions, bpm);
}

juce::String matchChannels(AudioDocument& doc, bool useRms)
{
    if (doc.getNumChannels() < 2)
        return {};

    const auto range = doc.getEffectiveRange();
    if (range.getLength() <= 0)
        return {};

    const int s = (int) range.getStart();
    const int n = (int) range.getLength();
    const auto& buf = doc.getBuffer();
    const float lvL = useRms ? buf.getRMSLevel(0, s, n) : buf.getMagnitude(0, s, n);
    const float lvR = useRms ? buf.getRMSLevel(1, s, n) : buf.getMagnitude(1, s, n);
    if (lvL < 1.0e-6f || lvR < 1.0e-6f)
        return {};   // one side is essentially silent -- nothing sane to match it to

    const int   quietCh = lvL < lvR ? 0 : 1;
    const float gain    = lvL < lvR ? (lvR / lvL) : (lvL / lvR);
    if (gain <= 1.0009f)   // < ~0.008 dB apart
        return "Channels already matched";

    doc.beginChange();
    auto working = doc.getBuffer();
    working.applyGain(quietCh, s, n, gain);
    doc.commitChange(std::move(working), "Match Channels");

    return juce::String::formatted("Matched: +%.1f dB to %s",
                                   juce::Decibels::gainToDecibels(gain),
                                   quietCh == 0 ? "Left" : "Right");
}

void convertToMono(AudioDocument& doc)
{
    const auto& src = doc.getBuffer();
    const int n  = src.getNumSamples();
    const int nc = src.getNumChannels();
    if (n <= 0 || nc < 2)
        return;

    juce::AudioBuffer<float> mono(1, n);
    mono.clear();
    for (int ch = 0; ch < nc; ++ch)
        mono.addFrom(0, 0, src, ch, 0, n, 1.0f / (float) nc);   // average -- headroom-safe downmix

    doc.channelFocus = AudioDocument::ChannelFocus::stereo;
    doc.beginChange();
    doc.commitChange(std::move(mono), "Convert to Mono");
}

void convertToStereo(AudioDocument& doc)
{
    const auto& src = doc.getBuffer();
    const int n = src.getNumSamples();
    if (n <= 0 || src.getNumChannels() != 1)
        return;

    juce::AudioBuffer<float> stereo(2, n);
    stereo.copyFrom(0, 0, src, 0, 0, n);
    stereo.copyFrom(1, 0, src, 0, 0, n);   // dual mono

    doc.channelFocus = AudioDocument::ChannelFocus::stereo;
    doc.beginChange();
    doc.commitChange(std::move(stereo), "Convert to Stereo");
}

} // namespace EditActions
