#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

/**
    Runs the *real* offline RubberBand stretch/pitch-shift (the same engine the destructive
    Stretch/Pitch tool uses -- see TimeStretchEngine) on a background thread, so
    WaveformDisplay can draw the actual processed waveform shape while the KnobRow's live
    Speed/Pitch/Stretch knobs are non-identity -- not just the stored audio rescaled to the
    right duration (see AudioDocument::getTimeScale()), which doesn't reflect how a real
    time-stretch reshapes transients and fine detail (they smear/soften at extreme ratios;
    a plain horizontal rescale of the original peaks doesn't).

    Debounced -- a knob has to sit still for `debounceMs` before a (re)compute starts, so a
    drag doesn't launch a RubberBand pass on every pixel of motion -- and always processed on
    a background juce::Thread, since an offline pass over a long file at an extreme ratio can
    take real time; the UI keeps showing whatever it already had until a new result lands.

    Message-thread-only surface: construct it, call update() once per UI tick (WaveformDisplay
    already has a 30 Hz timer), and read getProcessedBuffer() when update() returns true. All
    the cross-thread bookkeeping is internal.
*/
class WaveformStretchPreview : private juce::Thread
{
public:
    explicit WaveformStretchPreview(AudioDocument& document);
    ~WaveformStretchPreview() override;

    // Call once per tick. Returns true if getProcessedBuffer() has changed since the last
    // call (either a new processed buffer arrived, or the knobs returned to identity and it
    // was cleared) -- callers should rebuild whatever they cached from it.
    bool update();

    // Empty (getNumSamples() == 0) whenever the Speed/Pitch/Stretch knobs are all at
    // identity, or no processed buffer has been computed for the current knob position yet.
    // A non-empty buffer is the *actual* RubberBand output for the document's current full
    // content at the knob position `update()` last saw settle -- same channel count as the
    // document, its own (already stretch/pitch-adjusted) length.
    const juce::AudioBuffer<float>& getProcessedBuffer() const { return delivered; }

private:
    void run() override;
    void kickOffJob(double speed, double pitch, double stretch);

    AudioDocument& document;
    static constexpr uint32_t debounceMs = 250;

    // Message-thread-only.
    double lastSeenSpeed = 1.0, lastSeenPitch = 0.0, lastSeenStretch = 1.0;
    int lastSeenBufferVersion = -1;
    uint32_t settleDeadlineMs = 0;
    bool waitingToSettle = false;
    juce::AudioBuffer<float> delivered;

    // Cross-thread handoff: the message thread overwrites `request` (under `requestLock`) and
    // signals `wakeEvent`; the worker claims whatever's current when it wakes; if a newer
    // request already overwrote it by the time this one's done, it's simply superseded next
    // time the worker loops back round, rather than delivering an already-stale result. No
    // attempt to interrupt a RubberBand pass already in progress -- see the destructor.
    juce::WaitableEvent wakeEvent;
    juce::CriticalSection requestLock;
    bool haveRequest = false;
    juce::AudioBuffer<float> requestBuffer;
    double requestSpeed = 1.0, requestPitch = 0.0, requestStretch = 1.0, requestSampleRate = 44100.0;

    juce::CriticalSection resultLock;
    juce::AudioBuffer<float> pendingResult;
    std::atomic<bool> resultReady { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformStretchPreview)
};
