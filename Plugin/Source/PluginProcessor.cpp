#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>

namespace
{
    constexpr int kStateMagic     = 0x52335741;   // 'R3WA' - adds filter HP/LP slope (12/24dB)
    constexpr int kStateMagicR3W9 = 0x52335739;   // 'R3W9' - adds filterDrive
    constexpr int kStateMagicR3W8 = 0x52335738;   // 'R3W8' - filter is Base/Width, no drive
    constexpr int kStateMagicR3W7 = 0x52335737;   // 'R3W7' - mode/cutoff/res filter + playbackGainDb
    constexpr int kStateMagicR3W6 = 0x52335736;   // 'R3W6' - mode/cutoff/res filter, no gain
    constexpr int kStateMagicR3W5 = 0x52335735;   // 'R3W5' - no filter, no gain
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

    constexpr double ratioRampSeconds = 0.12;
    smoothedTimeRatio.reset(sampleRate, ratioRampSeconds);
    smoothedPitchScale.reset(sampleRate, ratioRampSeconds);
    smoothedTimeRatio.setCurrentAndTargetValue(1.0);
    smoothedPitchScale.setCurrentAndTargetValue(1.0);
    lastAppliedTimeRatio  = -1.0;
    lastAppliedPitchScale = -1.0;
    stretchRatioNeedsSnap = true;

    wasPlaying = false;
    stretcherPrimed = false;
    rtFinished = false;
    wasScrubbing = false;

    smoothedFilterBase.reset(sampleRate, 0.03);
    smoothedFilterWidth.reset(sampleRate, 0.03);
    smoothedFilterDrive.reset(sampleRate, 0.03);
    smoothedFilterBase.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterBase.load()));
    smoothedFilterWidth.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterWidth.load()));
    smoothedFilterDrive.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterDrive.load()));
    playbackFilter[0].reset();
    playbackFilter[1].reset();
    lastFilterEngaged = false;

    smoothedGain.reset(sampleRate, 0.02);
    smoothedGain.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain((float) juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb,
                                                            document.playbackGainDb.load()),
                                       (float) AudioDocument::kMinGainDb));
}

void R3WRKAudioProcessor::releaseResources()
{
    rtStretcher.reset();
    wasPlaying = false;
    stretcherPrimed = false;
    rtFinished = false;
    wasScrubbing = false;
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

    speed   = juce::jlimit(AudioDocument::kMinSpeed,   AudioDocument::kMaxSpeed,   speed);
    pitch   = juce::jlimit(AudioDocument::kMinPitch,   AudioDocument::kMaxPitch,   pitch);
    stretch = juce::jlimit(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, stretch);

    // Ramp the ratios instead of stepping them, so a knob drag mid-playback doesn't machine-gun
    // RubberBand with abrupt changes (that's the pops/crackle). Sampled once per block; snapped
    // straight to target on a fresh play pass so playback starts at the right ratio.
    const double targetTimeRatio  = stretch / juce::jmax(1.0e-4, speed);
    const double targetPitchScale = speed * std::pow(2.0, pitch / 12.0);
    if (stretchRatioNeedsSnap)
    {
        smoothedTimeRatio.setCurrentAndTargetValue(targetTimeRatio);
        smoothedPitchScale.setCurrentAndTargetValue(targetPitchScale);
        stretchRatioNeedsSnap = false;
    }
    else
    {
        smoothedTimeRatio.setTargetValue(targetTimeRatio);
        smoothedPitchScale.setTargetValue(targetPitchScale);
    }
    const double tr = smoothedTimeRatio.skip(numSamples);
    const double ps = smoothedPitchScale.skip(numSamples);
    if (std::abs(tr - lastAppliedTimeRatio) > 1.0e-4 * juce::jmax(1.0, tr))
    {
        rtStretcher->setTimeRatio(tr);
        lastAppliedTimeRatio = tr;
    }
    if (std::abs(ps - lastAppliedPitchScale) > 1.0e-4 * juce::jmax(1.0, ps))
    {
        rtStretcher->setPitchScale(ps);
        lastAppliedPitchScale = ps;
    }

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

// Scrub tool: a linear-interpolated, variable-rate (and reversible) read of the stored
// audio, at whatever rate WaveformDisplay's drag handling last computed from mouse
// velocity. No RubberBand involved -- pitch rising/falling with speed, and playing
// backwards cleanly at a negative rate, is exactly the point (a physical tape or turntable
// does the same); this is a much simpler DSP path than the pitch-corrected knobs.
void R3WRKAudioProcessor::renderScrub(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                      const juce::AudioBuffer<float>& docBuf)
{
    const int64_t docLen = docBuf.getNumSamples();
    if (docLen <= 1 || docBuf.getNumChannels() <= 0)
        return;

    // Defence in depth -- WaveformDisplay already clamps, but a stale or wild value here
    // would otherwise be audible as a shriek or a runaway read position.
    constexpr double maxSamplesPerSec = 44100.0 * 12.0;
    const double velocity = juce::jlimit(-maxSamplesPerSec, maxSamplesPerSec,
                                         document.scrubVelocity.load(std::memory_order_relaxed));
    const double perSample = velocity / juce::jmax(1.0, currentSampleRate);

    double pos = scrubReadPos;
    for (int i = 0; i < numSamples; ++i)
    {
        if (pos >= 0.0 && pos < (double) (docLen - 1))
        {
            const int64_t i0 = (int64_t) pos;
            const float frac = (float) (pos - (double) i0);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const int srcCh = juce::jmin(ch, docBuf.getNumChannels() - 1);
                const float* d = docBuf.getReadPointer(srcCh);
                out.setSample(ch, i, d[i0] + (d[i0 + 1] - d[i0]) * frac);
            }
        }
        // else: past either end -- leave this sample silent (buffer is already cleared)
        pos += perSample;
    }

    // Clamp so the cursor doesn't run away to +/-infinity while scrubbing off one end with
    // the mouse still held and moving.
    scrubReadPos = juce::jlimit(0.0, (double) (docLen - 1), pos);
    document.playhead.store((int64_t) scrubReadPos, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    if (desktopRecording.load(std::memory_order_relaxed))
    {
        // ScreenCaptureKit is doing the capture on its own queue (appendDesktopSamples) --
        // nothing here to record or monitor.
        buffer.clear();
        wasPlaying = false;
        return;
    }

    if (document.isRecording.load(std::memory_order_relaxed))
    {
        ensureRecordingCapacity(numCh, numSamples);
        for (int ch = 0; ch < numCh; ++ch)
            recordingAccumulator.copyFrom(ch, (int) recordingWritePos, buffer, ch, 0, numSamples);
        recordingWritePos += numSamples;

        // Feed the live scope: one peak min/max per 256-sample hop of the incoming block.
        constexpr int hop = 256;
        for (int s = 0; s < numSamples; s += hop)
        {
            const int nn = juce::jmin(hop, numSamples - s);
            float mn = 0.0f, mx = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch) + s, nn);
                mn = juce::jmin(mn, r.getStart());
                mx = juce::jmax(mx, r.getEnd());
            }
            const int wpos = document.scopeWritePos.load(std::memory_order_relaxed);
            document.scopeMin[wpos] = mn;
            document.scopeMax[wpos] = mx;
            document.scopeWritePos.store((wpos + 1) % AudioDocument::scopeSize, std::memory_order_release);
        }
        document.recordedSamples.store(recordingWritePos, std::memory_order_relaxed);

        wasPlaying = false;
        return; // pass input through unchanged so the user can monitor while recording
    }

    if (document.isScrubbing.load(std::memory_order_relaxed))
    {
        buffer.clear();

        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (stl.isLocked())
        {
            if (! wasScrubbing)   // just started -- pick up from wherever the drag began
                scrubReadPos = (double) document.playhead.load(std::memory_order_relaxed);
            renderScrub(buffer, numCh, numSamples, document.getBuffer());
        }

        applyPlaybackGain(buffer, numCh, numSamples);   // Gain knob rides scrub monitoring too
        captureOutput(buffer, numCh, numSamples);

        wasScrubbing = true;
        wasPlaying = false;   // so normal playback resets the stretcher cleanly if it resumes
        return;
    }
    wasScrubbing = false;

    if (document.isPlaying.load(std::memory_order_relaxed))
    {
        buffer.clear();

        const double speed   = document.playbackSpeed.load(std::memory_order_relaxed);
        const double pitch   = document.playbackPitch.load(std::memory_order_relaxed);
        const double stretch = document.playbackStretch.load(std::memory_order_relaxed);
        const bool engaged = knobsEngaged(speed, pitch, stretch);

        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (stl.isLocked())
        {
            auto& docBuf = document.getBuffer();
            const int64_t docLen = document.getNumSamples();
            const bool loop = document.loopEnabled.load(std::memory_order_relaxed);

            // Playback region: the selection if there is one (so Play plays the selected
            // range), otherwise the loop points, otherwise the whole clip. Loop loops it.
            // One getSelection() call so start+end always come from the same setSelection()
            // (they're packed into a single atomic on AudioDocument for exactly this reason).
            const auto sel  = document.getSelection();
            const int64_t selS = juce::jlimit((int64_t) 0, docLen, sel.getStart());
            const int64_t selE = juce::jlimit((int64_t) 0, docLen, sel.getEnd());
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
                    stretchRatioNeedsSnap = true;   // start at the current ratio, no 120ms slide in
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

        // Multi-mode filter, last in the chain -- applied whether the try-lock was held or not
        // (a filtered near-silent block is still correct), and to both direct + stretched paths.
        applyPlaybackFilter(buffer, numCh, numSamples, ! wasPlaying);
        applyPlaybackGain(buffer, numCh, numSamples);
        captureOutput(buffer, numCh, numSamples);

        wasPlaying = true;
        return;
    }

    wasPlaying = false;

    // Neither recording nor playing back: leave `buffer` untouched so the host's input
    // passes straight through -- except Auto-Record standby, which watches that same
    // pass-through input for a peak loud enough to cross autoRecordThresholdDb. Read-only:
    // it never touches `buffer` or starts recording itself, just flags it for the message
    // thread (see EditorToolbar::timerCallback) to act on -- see AudioDocument's comment.
    if (document.autoRecordEnabled.load(std::memory_order_relaxed)
        && ! document.autoRecordTriggered.load(std::memory_order_relaxed))
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch), numSamples);
            peak = juce::jmax(peak, std::abs(r.getStart()), std::abs(r.getEnd()));
        }
        const float peakDb = juce::Decibels::gainToDecibels(peak, -100.0f);
        if (peakDb >= (float) document.autoRecordThresholdDb.load(std::memory_order_relaxed))
            document.autoRecordTriggered.store(true, std::memory_order_relaxed);
    }

    // Keep the captured timeline continuous through idle gaps (in the Standalone the input is
    // muted, so this is silence -- but it stops a stop/start of playback mid-capture from
    // splicing the two parts together with no gap).
    captureOutput(buffer, numCh, numSamples);
}

void R3WRKAudioProcessor::applyPlaybackFilter(juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                              bool freshPlayPass)
{
    const double base01  = juce::jlimit(0.0, 1.0, document.filterBase.load(std::memory_order_relaxed));
    const double width01 = juce::jlimit(0.0, 1.0, document.filterWidth.load(std::memory_order_relaxed));
    const double drive01 = juce::jlimit(0.0, 1.0, document.filterDrive.load(std::memory_order_relaxed));
    const bool   engaged = r3wrk::filterEngaged(base01, width01, drive01);

    if (freshPlayPass || engaged != lastFilterEngaged)
    {
        playbackFilter[0].reset();
        playbackFilter[1].reset();

        if (freshPlayPass)
        {
            smoothedFilterBase.setCurrentAndTargetValue(base01);
            smoothedFilterWidth.setCurrentAndTargetValue(width01);
            smoothedFilterDrive.setCurrentAndTargetValue(drive01);
        }
        else if (engaged && ! lastFilterEngaged)
        {
            // Switched on mid-playback: ramp in from "wide open, no drive" so it eases in.
            smoothedFilterBase.setCurrentAndTargetValue(0.0);
            smoothedFilterWidth.setCurrentAndTargetValue(1.0);
            smoothedFilterDrive.setCurrentAndTargetValue(0.0);
        }
    }
    lastFilterEngaged = engaged;

    if (! engaged)
        return;

    smoothedFilterBase.setTargetValue(base01);
    smoothedFilterWidth.setTargetValue(width01);
    smoothedFilterDrive.setTargetValue(drive01);
    const double b = smoothedFilterBase.skip(numSamples);    // one coefficient set per block
    const double w = smoothedFilterWidth.skip(numSamples);
    const double d = smoothedFilterDrive.skip(numSamples);
    const double res = juce::jlimit(0.0, 1.0, document.filterResonance.load(std::memory_order_relaxed));
    const bool hp24 = document.filterHpSlope24.load(std::memory_order_relaxed);
    const bool lp24 = document.filterLpSlope24.load(std::memory_order_relaxed);

    for (int ch = 0; ch < juce::jmin(numCh, 2); ++ch)
    {
        playbackFilter[ch].setParams(b, w, res, d, currentSampleRate, hp24, lp24);
        playbackFilter[ch].processBlock(buffer.getWritePointer(ch), numSamples);
    }
}

void R3WRKAudioProcessor::applyPlaybackGain(juce::AudioBuffer<float>& buffer, int numCh, int numSamples)
{
    const double gainDb = document.playbackGainDb.load(std::memory_order_relaxed);
    const float target = std::abs(gainDb) > 1.0e-3
        ? juce::Decibels::decibelsToGain((float) juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, gainDb),
                                         (float) AudioDocument::kMinGainDb)
        : 1.0f;

    smoothedGain.setTargetValue(target);
    const float g0 = smoothedGain.getCurrentValue();
    const float g1 = smoothedGain.skip(numSamples);
    if (g0 == 1.0f && g1 == 1.0f)
        return;

    for (int ch = 0; ch < numCh; ++ch)
        buffer.applyGainRamp(ch, 0, numSamples, g0, g1);
}

void R3WRKAudioProcessor::captureOutput(const juce::AudioBuffer<float>& out, int numCh, int numSamples)
{
    if (! capturingOutput.load(std::memory_order_relaxed) || numSamples <= 0 || numCh <= 0)
        return;

    const int ch = juce::jlimit(1, 2, numCh);
    const int64_t needed = outputCaptureWritePos + numSamples;
    if (outputCaptureBuffer.getNumChannels() < ch || (int64_t) outputCaptureBuffer.getNumSamples() < needed)
    {
        int64_t cap = juce::jmax((int64_t) outputCaptureBuffer.getNumSamples(),
                                 (int64_t) (currentSampleRate * 30.0));
        while (cap < needed) cap *= 2;
        outputCaptureBuffer.setSize(ch, (int) cap, true, true, true);
    }
    for (int c = 0; c < ch; ++c)
        outputCaptureBuffer.copyFrom(c, (int) outputCaptureWritePos, out, juce::jmin(c, numCh - 1), 0, numSamples);
    outputCaptureWritePos += numSamples;
}

void R3WRKAudioProcessor::startOutputCapture()
{
    if (capturingOutput.load(std::memory_order_relaxed))
        return;
    outputCaptureWritePos = 0;
    outputCaptureBuffer.setSize(2, (int) juce::jmax(1.0, currentSampleRate * 30.0), false, true, true);
    capturingOutput.store(true, std::memory_order_relaxed);   // last: audio thread only appends once the buffer's ready
}

juce::File R3WRKAudioProcessor::stopOutputCaptureAndWrite(const juce::File& dest, const AudioSaveOptions& opts)
{
    capturingOutput.store(false, std::memory_order_relaxed);

    const int64_t n = outputCaptureWritePos;
    const int ch = outputCaptureBuffer.getNumChannels();
    if (n <= 0 || ch <= 0)
    {
        outputCaptureBuffer.setSize(0, 0);
        outputCaptureWritePos = 0;
        return {};
    }

    juce::AudioBuffer<float> finalBuf(ch, (int) n);
    for (int c = 0; c < ch; ++c)
        finalBuf.copyFrom(c, 0, outputCaptureBuffer, c, 0, (int) n);
    outputCaptureBuffer.setSize(0, 0);
    outputCaptureWritePos = 0;

    const juce::File out = dest.withFileExtension(opts.extension());
    if (! AudioDocument::writeAudioFile(std::move(finalBuf), currentSampleRate, out, opts))
        return {};
    return out;
}

void R3WRKAudioProcessor::startRecording()
{
    recordingWritePos = 0;
    document.resetRecordingScope();
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
    document.markAsOriginal();   // this take is the new "Revert to Original" baseline
}

//==============================================================================
void R3WRKAudioProcessor::startDesktopRecording()
{
    if (! DesktopAudioCapture::isSupported())
    {
        if (onDesktopStatus) onDesktopStatus("Desktop capture needs macOS 13 or later");
        return;
    }

    {
        const juce::ScopedLock sl(desktopRecLock);
        desktopRecBuffer.setSize(2, 0);
        desktopRecWritePos = 0;
        desktopRecChannels = 2;
        desktopRecRate = 48000.0;
    }
    document.resetRecordingScope();
    document.isPlaying = false;

    desktopCapture.start(
        [this](const float* const* d, int nc, int nf, double sr) { appendDesktopSamples(d, nc, nf, sr); },
        [this]
        {
            juce::MessageManager::callAsync([this]
            {
                desktopRecording = true;
                document.isRecording = true;
                document.notifyChanged();
            });
        },
        [this](juce::String msg)
        {
            juce::MessageManager::callAsync([this, msg]
            {
                const bool wasRunning = desktopRecording.exchange(false);
                document.isRecording = false;
                if (onDesktopStatus)
                    onDesktopStatus(msg.isNotEmpty() ? msg
                                                    : juce::String("Couldn't start desktop capture"));
                if (wasRunning)
                    finalizeDesktopRecording();   // stream died mid-take -- keep what landed
                document.notifyChanged();
            });
        });
}

void R3WRKAudioProcessor::stopDesktopRecording()
{
    desktopCapture.stop();          // sets running=false, stops the stream, drains the queue
    desktopRecording = false;
    document.isRecording = false;
    finalizeDesktopRecording();
}

void R3WRKAudioProcessor::appendDesktopSamples(const float* const* data, int numChannels, int numFrames, double sr)
{
    if (numFrames <= 0 || numChannels <= 0 || data == nullptr)
        return;

    const juce::ScopedLock sl(desktopRecLock);
    const int ch = juce::jlimit(1, 2, numChannels);
    desktopRecChannels = ch;
    desktopRecRate = sr > 0.0 ? sr : 48000.0;

    const int64_t needed = desktopRecWritePos + numFrames;
    if ((int64_t) desktopRecBuffer.getNumSamples() < needed || desktopRecBuffer.getNumChannels() < ch)
    {
        int64_t cap = juce::jmax((int64_t) desktopRecBuffer.getNumSamples(),
                                 (int64_t) (desktopRecRate * 8.0));
        while (cap < needed) cap *= 2;
        desktopRecBuffer.setSize(ch, (int) cap, true, true, true);
    }
    for (int c = 0; c < ch; ++c)
        if (data[c] != nullptr)
            desktopRecBuffer.copyFrom(c, (int) desktopRecWritePos, data[c], numFrames);
    desktopRecWritePos += numFrames;

    // Feed the live scope, same shape as the input-record path.
    constexpr int hop = 256;
    for (int s = 0; s < numFrames; s += hop)
    {
        const int nn = juce::jmin(hop, numFrames - s);
        float mn = 0.0f, mx = 0.0f;
        for (int c = 0; c < ch; ++c)
        {
            if (data[c] == nullptr) continue;
            const auto r = juce::FloatVectorOperations::findMinAndMax(data[c] + s, nn);
            mn = juce::jmin(mn, r.getStart());
            mx = juce::jmax(mx, r.getEnd());
        }
        const int wpos = document.scopeWritePos.load(std::memory_order_relaxed);
        document.scopeMin[wpos] = mn;
        document.scopeMax[wpos] = mx;
        document.scopeWritePos.store((wpos + 1) % AudioDocument::scopeSize, std::memory_order_release);
    }
    document.recordedSamples.store(desktopRecWritePos, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::finalizeDesktopRecording()
{
    juce::AudioBuffer<float> finalBuf;
    double rate = 48000.0;
    {
        const juce::ScopedLock sl(desktopRecLock);
        if (desktopRecWritePos <= 0)
        {
            if (onDesktopStatus) onDesktopStatus("No desktop audio captured");
            return;
        }
        finalBuf.setSize(desktopRecChannels, (int) desktopRecWritePos);
        for (int c = 0; c < desktopRecChannels; ++c)
            finalBuf.copyFrom(c, 0, desktopRecBuffer, c, 0, (int) desktopRecWritePos);
        rate = desktopRecRate;
        desktopRecBuffer.setSize(0, 0);
        desktopRecWritePos = 0;
    }

    document.beginChange();
    document.setSampleRate(rate);
    document.commitChange(std::move(finalBuf), "Record Desktop");
    document.loopStart = 0;
    document.loopEnd = document.getNumSamples();
    document.markAsOriginal();
    if (onDesktopStatus) onDesktopStatus("Desktop recording captured");
}

void R3WRKAudioProcessor::startPlayback()
{
    if (document.isEmpty())
        return;
    document.isRecording = false;

    const int64_t n = document.getNumSamples();
    int64_t p = document.playhead.load();
    const auto sel = document.getSelection();
    if (sel.getEnd() > sel.getStart())
    {
        const int64_t s = sel.getStart(), e = sel.getEnd();
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
    out.writeDouble(document.playbackSpeed.load());
    out.writeDouble(document.playbackPitch.load());
    out.writeDouble(document.playbackStretch.load());
    out.writeDouble(document.autoRecordThresholdDb.load());
    out.writeDouble(document.filterBase.load());
    out.writeDouble(document.filterWidth.load());
    out.writeDouble(document.filterResonance.load());
    out.writeDouble(document.filterDrive.load());
    out.writeBool(document.filterHpSlope24.load());
    out.writeBool(document.filterLpSlope24.load());
    out.writeDouble(document.playbackGainDb.load());

    auto& buf = document.getBuffer();
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        out.write(buf.getReadPointer(ch), (size_t) buf.getNumSamples() * sizeof(float));

    // Slice markers are deliberately NOT persisted -- they live only in AudioDocument for the
    // life of this plugin instance (per the user: "save for that session, not in permanent
    // memory"). Older state blobs may still carry a trailing marker list here; setStateInformation
    // just stops reading after the audio, so those extra bytes are harmless.
}

void R3WRKAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in(data, (size_t) sizeInBytes, false);
    const int magic = in.readInt();
    if (magic != kStateMagic && magic != kStateMagicR3W9 && magic != kStateMagicR3W8
        && magic != kStateMagicR3W7 && magic != kStateMagicR3W6 && magic != kStateMagicR3W5)
        return;
    const bool hasBaseWidthFilter = (magic == kStateMagic || magic == kStateMagicR3W9
                                     || magic == kStateMagicR3W8);                          // R3W8+
    const bool hasDriveField      = (magic == kStateMagic || magic == kStateMagicR3W9);     // R3W9+
    const bool hasSlopeField      = (magic == kStateMagic);                                 // R3WA+
    const bool hasOldModeFilter   = (magic == kStateMagicR3W7 || magic == kStateMagicR3W6);
    const bool hasGainField       = (magic == kStateMagic || magic == kStateMagicR3W9
                                     || magic == kStateMagicR3W8 || magic == kStateMagicR3W7);

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
    double thresh = in.readDouble();

    double fBase = 0.0, fWidth = 1.0, fRes = 0.0, fDrive = 0.0, gDb = 0.0;
    bool fHp24 = false, fLp24 = false;
    if (hasBaseWidthFilter)
    {
        fBase  = in.readDouble();
        fWidth = in.readDouble();
        fRes   = in.readDouble();
        if (hasDriveField)
            fDrive = in.readDouble();
        if (hasSlopeField)
        {
            fHp24 = in.readBool();
            fLp24 = in.readBool();
        }
    }
    else if (hasOldModeFilter)
    {
        // Migrate the pre-R3W8 mode/cutoff filter onto Base/Width so a returning session
        // keeps roughly the same sound. Notch (mode 4) has no Base/Width equivalent -> open.
        // The old single-biquad resonance doesn't map onto the new two-edge Q, so it's
        // dropped to 0 (still read, to keep the stream position for the gain field).
        const int    oldMode = in.readInt();
        const double oldCut  = in.readDouble();
        in.readDouble();          // old filterResonance -- consumed but not carried over
        fRes = 0.0;
        const double pos = r3wrk::filterHzToPos(oldCut > 0.0 ? oldCut : 1000.0);
        switch (oldMode)
        {
            case 1: fBase = 0.0;                              fWidth = pos;  break;  // low-pass
            case 2: fBase = pos;                              fWidth = 1.0;  break;  // high-pass
            case 3: fBase = juce::jlimit(0.0, 1.0, pos - 0.06); fWidth = 0.12; break;  // band-pass
            default: fBase = 0.0;                             fWidth = 1.0;  break;  // off / notch
        }
    }
    if (hasGainField)
        gDb = in.readDouble();

    if (numCh <= 0 || numCh > kMaxStateChannels || numSamples < 0 || numSamples > 0x7fffffff)
        return;

    const int64_t audioBytes = numSamples * numCh * (int64_t) sizeof(float);
    if (audioBytes > (int64_t) (in.getTotalLength() - in.getPosition()))
        return;   // truncated / corrupt state -- don't try to read past the end

    juce::AudioBuffer<float> loaded(numCh, (int) numSamples);
    for (int ch = 0; ch < numCh; ++ch)
        in.read(loaded.getWritePointer(ch), (int) ((size_t) numSamples * sizeof(float)));

    // Slice markers are not restored from state -- they're session-only (see getStateInformation).
    // Any trailing marker bytes an older blob wrote are simply left unread.

    document.newEmptyDocument(numCh, sr);
    document.beginChange();
    document.commitChange(std::move(loaded), "Load State");
    document.loopStart = lStart;
    document.loopEnd = lEnd;
    document.loopEnabled = lEnabled;
    document.setSelection(selStartS, selEndS);   // clamps to the loaded length

    document.playbackSpeed.store(juce::jlimit(AudioDocument::kMinSpeed, AudioDocument::kMaxSpeed, spd > 0.0 ? spd : 1.0));
    document.playbackPitch.store(juce::jlimit(AudioDocument::kMinPitch, AudioDocument::kMaxPitch, pch));
    document.playbackStretch.store(juce::jlimit(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, str > 0.0 ? str : 1.0));
    document.autoRecordThresholdDb.store(juce::jlimit(-60.0, 0.0, thresh));
    document.filterBase.store(juce::jlimit(0.0, 1.0, fBase));
    document.filterWidth.store(juce::jlimit(0.0, 1.0, fWidth));
    document.filterResonance.store(juce::jlimit(0.0, 1.0, fRes));
    document.filterDrive.store(juce::jlimit(0.0, 1.0, fDrive));
    document.filterHpSlope24.store(fHp24);
    document.filterLpSlope24.store(fLp24);
    document.playbackGainDb.store(juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, gDb));

    document.clearSliceMarkers();   // session-only; a restored document starts with no markers

    document.markAsOriginal();   // the restored session is the new "Revert to Original" baseline
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new R3WRKAudioProcessor();
}
