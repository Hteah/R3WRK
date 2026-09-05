#include "WaveformStretchPreview.h"
#include "TimeStretchEngine.h"
#include <cmath>

WaveformStretchPreview::WaveformStretchPreview(AudioDocument& doc)
    : juce::Thread("R3WRK stretch preview"), document(doc)
{
    startThread();
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
        if (delivered.getNumSamples() > 0)
        {
            delivered.setSize(0, 0);
            return true;
        }
        return false;
    }

    if (waitingToSettle && juce::Time::getMillisecondCounter() >= settleDeadlineMs)
    {
        waitingToSettle = false;
        kickOffJob(speed, pitch, stretch);
    }

    if (resultReady.exchange(false))
    {
        const juce::ScopedLock sl(resultLock);
        delivered = std::move(pendingResult);
        return true;
    }
    return false;
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

        // Same time-ratio / pitch-scale mapping the real-time playback engine uses (see
        // PluginProcessor::renderPlaybackStretched) -- speed acts like tape speed (changes
        // both duration and pitch), stretch is pure duration, pitch is the extra shift on
        // top -- so this offline pass reproduces what you'll actually hear, not an
        // approximation of it. TimeStretchEngine's offline engine takes semitones, not a
        // pitch-scale ratio, hence the log2 conversion back from the real-time engine's units.
        const double timeRatio = stretch / juce::jmax(0.0001, speed);
        const double semitones = 12.0 * std::log2(juce::jmax(0.0001, speed)) + pitch;
        auto processed = TimeStretchEngine::process(raw, sr, timeRatio, semitones);

        if (processed.getNumSamples() > 0)
        {
            const juce::ScopedLock sl(resultLock);
            pendingResult = std::move(processed);
            resultReady = true;
        }
    }
}
