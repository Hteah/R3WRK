#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int kStateMagic = 0x52335731;   // 'R3W1' - session-state format tag
    constexpr int kMaxStateChannels = 32;
}

R3WRKAudioProcessor::R3WRKAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    document.newEmptyDocument(2, 44100.0);
}

R3WRKAudioProcessor::~R3WRKAudioProcessor() = default;

void R3WRKAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
    if (document.isEmpty())
        document.setSampleRate(sampleRate);
}

void R3WRKAudioProcessor::releaseResources()
{
}

bool R3WRKAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    auto mono = juce::AudioChannelSet::mono();
    auto stereo = juce::AudioChannelSet::stereo();
    auto in = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return in == mono || in == stereo;
}

void R3WRKAudioProcessor::ensureRecordingCapacity(int numChannels, int64_t additionalSamples)
{
    int64_t needed = recordingWritePos + additionalSamples;
    if (recordingAccumulator.getNumChannels() != numChannels || (int64_t) recordingAccumulator.getNumSamples() < needed)
    {
        int64_t newCapacity = juce::jmax((int64_t) recordingAccumulator.getNumSamples(),
                                          (int64_t) (currentSampleRate * 4.0));
        while (newCapacity < needed)
            newCapacity *= 2;
        recordingAccumulator.setSize(numChannels, (int) newCapacity, true, true, true);
    }
}

void R3WRKAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    if (document.isRecording.load(std::memory_order_relaxed))
    {
        ensureRecordingCapacity(numCh, numSamples);
        for (int ch = 0; ch < numCh; ++ch)
            recordingAccumulator.copyFrom(ch, (int) recordingWritePos, buffer, ch, 0, numSamples);
        recordingWritePos += numSamples;
        return; // pass input through unchanged so the user can monitor while recording
    }

    if (document.isPlaying.load(std::memory_order_relaxed))
    {
        buffer.clear();

        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (stl.isLocked())
        {
            auto& docBuf = document.getBuffer();
            const int64_t docLen = document.getNumSamples();
            const bool loop = document.loopEnabled.load(std::memory_order_relaxed);

            // Playback region: the selection if there is one (so Play plays the selected
            // range), otherwise the loop points, otherwise the whole clip. Loop loops it.
            const int64_t selS = juce::jlimit((int64_t) 0, docLen, document.getSelectionStart());
            const int64_t selE = juce::jlimit((int64_t) 0, docLen, document.getSelectionEnd());
            const int64_t loopStartS = document.loopStart.load(std::memory_order_relaxed);
            const int64_t loopEndS = document.loopEnd.load(std::memory_order_relaxed);

            int64_t regionStart = 0, regionEnd = docLen;
            if (selE > selS)                              { regionStart = selS;       regionEnd = selE; }
            else if (loop && loopEndS > loopStartS)       { regionStart = loopStartS; regionEnd = loopEndS; }

            int64_t pos = document.playhead.load(std::memory_order_relaxed);
            if (pos < regionStart || pos >= regionEnd)
                pos = regionStart;                       // snap a stray playhead into the region

            int written = 0;
            while (written < numSamples && docBuf.getNumChannels() > 0)
            {
                if (pos >= regionEnd)
                {
                    if (loop) { pos = regionStart; continue; }
                    break;
                }

                int chunk = (int) juce::jmin((int64_t) (numSamples - written), regionEnd - pos);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    int srcCh = juce::jmin(ch, docBuf.getNumChannels() - 1);
                    buffer.copyFrom(ch, written, docBuf, srcCh, (int) pos, chunk);
                }
                pos += chunk;
                written += chunk;
            }

            document.playhead.store(pos, std::memory_order_relaxed);

            if (! loop && pos >= regionEnd)
                document.isPlaying.store(false, std::memory_order_relaxed);
        }
        return;
    }

    // Neither recording nor playing back: leave `buffer` untouched so the host's input
    // passes straight through.
}

void R3WRKAudioProcessor::startRecording()
{
    recordingWritePos = 0;
    int chans = getTotalNumInputChannels() > 0 ? getTotalNumInputChannels() : 2;
    recordingAccumulator.setSize(chans, (int) juce::jmax(1.0, currentSampleRate * 4.0), false, true, true);
    document.isPlaying = false;
    document.isRecording = true;
}

void R3WRKAudioProcessor::stopRecording()
{
    document.isRecording = false;

    if (recordingWritePos <= 0)   // stopped before any audio was captured -- leave the document alone
        return;

    juce::AudioBuffer<float> finalBuffer(recordingAccumulator.getNumChannels(), (int) recordingWritePos);
    for (int ch = 0; ch < finalBuffer.getNumChannels(); ++ch)
        finalBuffer.copyFrom(ch, 0, recordingAccumulator, ch, 0, (int) recordingWritePos);

    document.beginChange();
    document.setSampleRate(currentSampleRate);
    document.commitChange(std::move(finalBuffer), "Record");
    document.loopStart = 0;
    document.loopEnd = document.getNumSamples();
}

void R3WRKAudioProcessor::startPlayback()
{
    if (document.isEmpty())
        return;
    document.isRecording = false;

    const int64_t n = document.getNumSamples();
    int64_t p = document.playhead.load();
    if (document.hasSelection())
    {
        const int64_t s = document.getSelectionStart();
        const int64_t e = document.getSelectionEnd();
        if (p < s || p >= e)          // start from the selection unless the cursor is inside it
            p = s;
    }
    else if (p >= n)
    {
        p = 0;
    }
    document.playhead = p;
    document.isPlaying = true;
}

void R3WRKAudioProcessor::stopPlayback()
{
    document.isPlaying = false;
}

juce::AudioProcessorEditor* R3WRKAudioProcessor::createEditor()
{
    return new R3WRKAudioProcessorEditor(*this);
}

void R3WRKAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out(destData, false);
    out.writeInt(kStateMagic);
    out.writeDouble(document.getSampleRate());
    out.writeInt(document.getNumChannels());
    out.writeInt64(document.getNumSamples());
    out.writeInt64(document.loopStart.load());
    out.writeInt64(document.loopEnd.load());
    out.writeBool(document.loopEnabled.load());

    auto& buf = document.getBuffer();
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        out.write(buf.getReadPointer(ch), (size_t) buf.getNumSamples() * sizeof(float));
}

void R3WRKAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in(data, (size_t) sizeInBytes, false);
    if (in.readInt() != kStateMagic)
        return;

    double sr = in.readDouble();
    int numCh = in.readInt();
    int64_t numSamples = in.readInt64();
    int64_t lStart = in.readInt64();
    int64_t lEnd = in.readInt64();
    bool lEnabled = in.readBool();

    if (numCh <= 0 || numCh > kMaxStateChannels || numSamples < 0 || numSamples > 0x7fffffff)
        return;

    const int64_t audioBytes = numSamples * numCh * (int64_t) sizeof(float);
    if (audioBytes > (int64_t) (in.getTotalLength() - in.getPosition()))
        return;   // truncated / corrupt state -- don't try to read past the end

    juce::AudioBuffer<float> loaded(numCh, (int) numSamples);
    for (int ch = 0; ch < numCh; ++ch)
        in.read(loaded.getWritePointer(ch), (int) ((size_t) numSamples * sizeof(float)));

    document.newEmptyDocument(numCh, sr);
    document.beginChange();
    document.commitChange(std::move(loaded), "Load State");
    document.loopStart = lStart;
    document.loopEnd = lEnd;
    document.loopEnabled = lEnabled;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new R3WRKAudioProcessor();
}
