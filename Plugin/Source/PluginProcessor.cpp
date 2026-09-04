#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>

namespace
{
    constexpr int kStateMagic = 0x52335733;   // 'R3W3' - session-state format tag
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

bool R3WRKAudioProcessor::knobsEngaged(double speed, double pitch, double stretch)
{
    return std::abs(speed - 1.0) > 1.0e-4
        || std::abs(pitch) > 1.0e-4
        || std::abs(stretch - 1.0) > 1.0e-4;
}

void R3WRKAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    if (document.isEmpty())
        document.setSampleRate(sampleRate);

    rtChannels = juce::jlimit(1, 2, juce::jmax(1, getMainBusNumOutputChannels()));

    using RB = RubberBand::RubberBandStretcher;
    rtStretcher = std::make_unique<RB>((size_t) sampleRate, (size_t) rtChannels,
                                       RB::OptionProcessRealTime | RB::OptionPitchHighConsistency);

    const int inScratch = juce::jmax(8192, juce::jmax(0, samplesPerBlock) * 8);
    rtScratchIn.setSize(rtChannels, inScratch);
    rtScratchOut.setSize(rtChannels, juce::jmax(8192, juce::jmax(0, samplesPerBlock) * 2));
    rtStretcher->setMaxProcessSize((size_t) inScratch);

    wasPlaying = false;
    stretcherPrimed = false;
    rtFinished = false;
}

void R3WRKAudioProcessor::releaseResources()
{
    rtStretcher.reset();
    wasPlaying = false;
    stretcherPrimed = false;
    rtFinished = false;
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

//==============================================================================
void R3WRKAudioProcessor::renderPlaybackDirect(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                               const juce::AudioBuffer<float>& docBuf,
                                               int64_t& pos, int64_t regionStart, int64_t regionEnd, bool loop)
{
    int written = 0;
    while (written < numSamples && docBuf.getNumChannels() > 0)
    {
        if (pos >= regionEnd)
        {
            if (loop && regionEnd > regionStart) { pos = regionStart; continue; }
            break;
        }

        int chunk = (int) juce::jmin((int64_t) (numSamples - written), regionEnd - pos);
        for (int ch = 0; ch < numCh; ++ch)
        {
            int srcCh = juce::jmin(ch, docBuf.getNumChannels() - 1);
            out.copyFrom(ch, written, docBuf, srcCh, (int) pos, chunk);
        }
        pos += chunk;
        written += chunk;
    }

    document.playhead.store(pos, std::memory_order_relaxed);
    if (! loop && pos >= regionEnd)
        document.isPlaying.store(false, std::memory_order_relaxed);
}

// Real-time tape / pitch / stretch path. All three knobs are one RubberBand pass:
//   timeRatio  = stretch / speed   (speed compresses time like tape, stretch dilates it)
//   pitchScale = speed * 2^(pitch/12)   (pitch tracks the tape speed, then the extra shift;
//                                        stretch does NOT touch pitch)
void R3WRKAudioProcessor::renderPlaybackStretched(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                                  const juce::AudioBuffer<float>& docBuf,
                                                  int64_t& pos, int64_t regionStart, int64_t regionEnd,
                                                  bool loop, double speed, double pitch, double stretch)
{
    if (rtStretcher == nullptr || docBuf.getNumChannels() <= 0 || regionEnd <= regionStart)
        return;

    speed   = juce::jlimit(kMinSpeed,   kMaxSpeed,   speed);
    pitch   = juce::jlimit(kMinPitch,   kMaxPitch,   pitch);
    stretch = juce::jlimit(kMinStretch, kMaxStretch, stretch);
    rtStretcher->setTimeRatio(stretch / speed);
    rtStretcher->setPitchScale(speed * std::pow(2.0, pitch / 12.0));

    const int rc = rtChannels;
    const int inCap  = rtScratchIn.getNumSamples();
    const int outCap = rtScratchOut.getNumSamples();

    int produced = 0;
    int guard = numSamples * 4 + 64;   // hard cap on iterations, just in case

    while (produced < numSamples && --guard > 0)
    {
        const int avail = (int) rtStretcher->available();
        if (avail > 0)
        {
            const int n = juce::jmin(avail, numSamples - produced, outCap);
            float* op[2] = { rtScratchOut.getWritePointer(0),
                             rtScratchOut.getWritePointer(rc > 1 ? 1 : 0) };
            rtStretcher->retrieve(op, (size_t) n);
            for (int ch = 0; ch < numCh; ++ch)
                out.copyFrom(ch, produced, rtScratchOut, juce::jmin(ch, rc - 1), 0, n);
            produced += n;
            continue;
        }

        if (rtFinished)   // final block already sent and the stream has drained
        {
            document.isPlaying.store(false, std::memory_order_relaxed);
            break;
        }

        int req = (int) rtStretcher->getSamplesRequired();
        req = juce::jlimit(1, inCap, req > 0 ? req : 256);

        int gathered = 0;
        bool regionEnded = false;
        while (gathered < req)
        {
            if (pos >= regionEnd)
            {
                if (loop) { pos = regionStart; }
                else      { regionEnded = true; break; }
            }
            int chunk = (int) juce::jmin((int64_t) (req - gathered), regionEnd - pos);
            for (int ch = 0; ch < rc; ++ch)
            {
                int srcCh = juce::jmin(ch, docBuf.getNumChannels() - 1);
                rtScratchIn.copyFrom(ch, gathered, docBuf, srcCh, (int) pos, chunk);
            }
            pos += chunk;
            gathered += chunk;
        }
        for (int ch = 0; ch < rc; ++ch)
            if (gathered < req)
                rtScratchIn.clear(ch, gathered, req - gathered);

        const float* ip[2] = { rtScratchIn.getReadPointer(0),
                               rtScratchIn.getReadPointer(rc > 1 ? 1 : 0) };
        const bool finalNow = regionEnded && ! loop;
        rtStretcher->process(ip, (size_t) req, finalNow);
        if (finalNow)
            rtFinished = true;
    }

    document.playhead.store(pos, std::memory_order_relaxed);
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
        wasPlaying = false;
        return; // pass input through unchanged so the user can monitor while recording
    }

    if (document.isPlaying.load(std::memory_order_relaxed))
    {
        buffer.clear();

        const double speed   = playbackSpeed.load(std::memory_order_relaxed);
        const double pitch   = playbackPitch.load(std::memory_order_relaxed);
        const double stretch = playbackStretch.load(std::memory_order_relaxed);
        const bool engaged = knobsEngaged(speed, pitch, stretch);

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
            const int64_t loopEndS   = document.loopEnd.load(std::memory_order_relaxed);

            int64_t regionStart = 0, regionEnd = docLen;
            if (selE > selS)                              { regionStart = selS;       regionEnd = selE; }
            else if (loop && loopEndS > loopStartS)       { regionStart = loopStartS; regionEnd = loopEndS; }

            int64_t pos = document.playhead.load(std::memory_order_relaxed);
            if (pos < regionStart || pos >= regionEnd)
                pos = regionStart;                       // snap a stray playhead into the region

            // Reset the stretcher at the start of a play pass, or when the knobs cross the
            // bypass/engaged line, so no stale tail leaks in.
            if (engaged)
            {
                if (rtStretcher != nullptr && (! wasPlaying || ! stretcherPrimed))
                {
                    rtStretcher->reset();
                    stretcherPrimed = true;
                    rtFinished = false;
                }
                renderPlaybackStretched(buffer, numCh, numSamples, docBuf, pos,
                                        regionStart, regionEnd, loop, speed, pitch, stretch);
            }
            else
            {
                stretcherPrimed = false;
                rtFinished = false;
                renderPlaybackDirect(buffer, numCh, numSamples, docBuf, pos,
                                     regionStart, regionEnd, loop);
            }
        }

        wasPlaying = true;
        return;
    }

    wasPlaying = false;

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
    out.writeInt64(document.getSelectionStart());
    out.writeInt64(document.getSelectionEnd());
    out.writeDouble(playbackSpeed.load());
    out.writeDouble(playbackPitch.load());
    out.writeDouble(playbackStretch.load());

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
    int64_t selStartS = in.readInt64();
    int64_t selEndS = in.readInt64();
    double spd = in.readDouble();
    double pch = in.readDouble();
    double str = in.readDouble();

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
    document.setSelection(selStartS, selEndS);   // clamps to the loaded length

    playbackSpeed.store(juce::jlimit(kMinSpeed, kMaxSpeed, spd > 0.0 ? spd : 1.0));
    playbackPitch.store(juce::jlimit(kMinPitch, kMaxPitch, pch));
    playbackStretch.store(juce::jlimit(kMinStretch, kMaxStretch, str > 0.0 ? str : 1.0));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new R3WRKAudioProcessor();
}
