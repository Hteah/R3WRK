#include "WaveformStretchPreview.h"
#include "TimeStretchEngine.h"
#include <cmath>

WaveformStretchPreview::WaveformStretchPreview(AudioDocument& doc)
    : juce::Thread("R3WRK stretch preview"), document(doc)
{
    // Normal priority: the pass only runs while playback is stopped (see update()), so there's
    // no real-time thread to protect -- and low priority just made it drag on a busy machine.
    startThread(juce::Thread::Priority::normal);
}

WaveformStretchPreview::~WaveformStretchPreview()
{
    // RubberBand's offline process() isn't interruptible mid-call, so this can't force an
    // in-flight pass to abandon early -- a generous timeout instead. Only a real risk for a
    // very long clip at an extreme ratio outliving the editor being closed; ordinary use
    // (this is a sample editor, not a multi-hour multitrack one) finishes well within it.
    stopThread(10000);
}

bool WaveformStretchPreview::update()
{
    const double speed = document.playbackSpeed.load();
    const double pitch = document.playbackPitch.load();
    const double stretch = document.playbackStretch.load();
    const int bufferVersion = document.getBufferVersion();

    const bool identity = std::abs(speed - 1.0) < 1.0e-9
                        && std::abs(pitch) < 1.0e-9
                        && std::abs(stretch - 1.0) < 1.0e-9;

    const bool changed = std::abs(speed - lastSeenSpeed) > 1.0e-9
                      || std::abs(pitch - lastSeenPitch) > 1.0e-9
                      || std::abs(stretch - lastSeenStretch) > 1.0e-9
                      || bufferVersion != lastSeenBufferVersion;
    if (changed)
    {
        lastSeenSpeed = speed;
        lastSeenPitch = pitch;
        lastSeenStretch = stretch;
        lastSeenBufferVersion = bufferVersion;
        settleDeadlineMs = juce::Time::getMillisecondCounter() + debounceMs;
        waitingToSettle = true;
    }

    if (identity)
    {
        // Nothing to preview -- the caller should just draw the stored buffer, rescaled.
        waitingToSettle = false;
        if (! peakMin.empty())
        {
            peakMin.clear();
            peakMax.clear();
            processedLength = 0;
            return true;
        }
        return false;
    }

    // Don't compute the preview while audio is playing: kickOffJob() copies the whole document
    // buffer under document.getLock(), which can make processBlock's try-lock fail for a block
    // (-> a dropped block -> a click), and the offline RubberBand pass would be competing with
    // the real-time stretcher for CPU. The knob move stays pending (waitingToSettle) and the
    // preview refreshes once playback stops.
    if (waitingToSettle && ! document.isPlaying.load()
        && juce::Time::getMillisecondCounter() >= settleDeadlineMs)
    {
        waitingToSettle = false;
        kickOffJob(speed, pitch, stretch);
    }

    if (resultReady.exchange(false))
    {
        const juce::ScopedLock sl(resultLock);
        peakMin = std::move(pendingResult.peakMin);
        peakMax = std::move(pendingResult.peakMax);
        processedLength = pendingResult.length;
        binSize = juce::jmax(1, pendingResult.binSize);
        return true;
    }
    return false;
}

void WaveformStretchPreview::getPeakRange(int channel, int64_t p0, int64_t p1, float& outMin, float& outMax) const
{
    outMin = 0.0f;
    outMax = 0.0f;
    if (channel < 0 || channel >= (int) peakMin.size())
        return;

    const auto& mn = peakMin[(size_t) channel];
    const auto& mx = peakMax[(size_t) channel];
    const int nBins = (int) mn.size();
    if (nBins <= 0)
        return;

    p0 = juce::jlimit((int64_t) 0, processedLength, p0);
    p1 = juce::jlimit(p0, processedLength, p1);
    if (p1 <= p0)
        return;

    const int b0 = juce::jlimit(0, nBins - 1, (int) (p0 / binSize));
    const int b1 = juce::jlimit(b0, nBins - 1, (int) ((p1 - 1) / binSize));
    for (int b = b0; b <= b1; ++b)
    {
        outMin = juce::jmin(outMin, mn[(size_t) b]);
        outMax = juce::jmax(outMax, mx[(size_t) b]);
    }
}

int WaveformStretchPreview::chooseBinSize(int64_t processedSamples)
{
    int bs = minBinSize;
    while (processedSamples / bs > maxBinsPerChannel && bs < 4096)
        bs *= 2;
    return bs;
}

void WaveformStretchPreview::kickOffJob(double speed, double pitch, double stretch)
{
    const juce::ScopedLock sl(requestLock);
    requestSpeed = speed;
    requestPitch = pitch;
    requestStretch = stretch;
    requestSampleRate = document.getSampleRate();
    {
        const juce::ScopedLock sl2(document.getLock());
        requestBuffer.makeCopyOf(document.getBuffer());
    }
    haveRequest = true;
    wakeEvent.signal();
}

void WaveformStretchPreview::run()
{
    while (! threadShouldExit())
    {
        jobRunning.store(false, std::memory_order_relaxed);
        wakeEvent.wait(500);   // periodic wake so threadShouldExit() still gets checked at shutdown
        if (threadShouldExit())
            break;

        juce::AudioBuffer<float> raw;
        double speed = 1.0, pitch = 0.0, stretch = 1.0, sr = 44100.0;
        {
            const juce::ScopedLock sl(requestLock);
            if (! haveRequest)
                continue;
            haveRequest = false;
            raw = std::move(requestBuffer);
            speed = requestSpeed;
            pitch = requestPitch;
            stretch = requestStretch;
            sr = requestSampleRate;
        }

        if (raw.getNumSamples() <= 0)
            continue;

        jobRunning.store(true, std::memory_order_relaxed);   // WaveformDisplay shows a spinner

        // Same time-ratio / pitch-scale mapping the real-time playback engine uses (see
        // PluginProcessor::renderPlaybackStretched) -- speed acts like tape speed (changes
        // both duration and pitch), stretch is pure duration, pitch is the extra shift on
        // top -- so this offline pass reproduces what you'll actually hear, not an
        // approximation of it. TimeStretchEngine's offline engine takes semitones, not a
        // pitch-scale ratio, hence the log2 conversion back from the real-time engine's units.
        const double timeRatio = stretch / juce::jmax(0.0001, speed);
        const double semitones = 12.0 * std::log2(juce::jmax(0.0001, speed)) + pitch;
        auto processed = TimeStretchEngine::process(raw, sr, timeRatio, semitones,
                                                    /*fastPreview*/ true);   // just for the drawn shape

        if (processed.getNumSamples() <= 0)
            continue;

        // Bin into a peak (min/max per bin) cache here, on this thread, same approach as
        // WaveformDisplay::rebuildPeakCache() -- see the class comment for why: the processed
        // buffer itself can be huge at an extreme ratio, and the message thread must never be
        // the one scanning it. Bin size adapts to the length (chooseBinSize) so a normal-ratio
        // preview stays fine enough to zoom into without going blocky.
        Result result;
        const int numCh = processed.getNumChannels();
        const int64_t total = processed.getNumSamples();
        const int bs = chooseBinSize(total);
        const int nBins = (int) ((total + bs - 1) / bs);
        result.length = total;
        result.binSize = bs;
        result.peakMin.assign((size_t) numCh, std::vector<float>((size_t) juce::jmax(0, nBins), 0.0f));
        result.peakMax.assign((size_t) numCh, std::vector<float>((size_t) juce::jmax(0, nBins), 0.0f));

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* d = processed.getReadPointer(ch);
            auto& mn = result.peakMin[(size_t) ch];
            auto& mx = result.peakMax[(size_t) ch];
            for (int b = 0; b < nBins; ++b)
            {
                const int64_t s0 = (int64_t) b * bs;
                const int64_t s1 = juce::jmin(total, s0 + bs);
                const auto r = juce::FloatVectorOperations::findMinAndMax(d + s0, (int) (s1 - s0));
                mn[(size_t) b] = r.getStart();
                mx[(size_t) b] = r.getEnd();
            }

            if (threadShouldExit())   // shutting down mid-binning -- don't bother delivering
                return;
        }

        {
            const juce::ScopedLock sl(resultLock);
            pendingResult = std::move(result);
            resultReady = true;
        }
    }
}
