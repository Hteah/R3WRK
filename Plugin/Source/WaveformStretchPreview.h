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

    The background thread also bins the processed audio into a peak (min/max per
    `peakBinSize` samples) cache before delivering it, the same way WaveformDisplay's own
    rebuildPeakCache() does for the raw buffer -- *not* handing back the raw processed audio
    for the message thread to scan itself. At an extreme ratio the processed buffer can run to
    tens of millions of samples; scanning that directly on the message thread once it lands
    would block the UI (mouse events included -- this is what broke making selections, and
    likely dropped/delayed transport clicks too) for as long as the scan takes, on top of the
    RubberBand pass itself. The peak cache keeps the message-thread side of a rebuild to the
    same, already-fine cost as the existing raw-buffer path, however long the processed audio
    actually is.

    Message-thread-only surface: construct it, call update() once per UI tick (WaveformDisplay
    already has a 30 Hz timer), and read getPeakRange()/hasPreview()/getProcessedLength() when
    update() returns true. All the cross-thread bookkeeping is internal.
*/
class WaveformStretchPreview : private juce::Thread
{
public:
    explicit WaveformStretchPreview(AudioDocument& document);
    ~WaveformStretchPreview() override;

    // Call once per tick. Returns true if the delivered preview has changed since the last
    // call (either a new one arrived, or the knobs returned to identity and it was cleared) --
    // callers should rebuild whatever they cached from it.
    bool update();

    // False whenever the Speed/Pitch/Stretch knobs are all at identity, or no preview has
    // been computed for the current knob position yet.
    bool hasPreview() const { return processedLength > 0 && ! peakMin.empty(); }

    // The *actual* RubberBand output length (in its own, already stretch/pitch-adjusted
    // sample domain) for the document's current content at the knob position update() last
    // saw settle. Only meaningful when hasPreview().
    int64_t getProcessedLength() const { return processedLength; }
    int getNumChannels() const { return (int) peakMin.size(); }

    // Min/max over processed-domain samples [p0, p1) for channel `channel`, via the peak
    // cache -- same bin-scanning approach as WaveformDisplay's raw-buffer fallback path.
    // Leaves outMin/outMax at 0 (silence) if out of range or channel is invalid.
    void getPeakRange(int channel, int64_t p0, int64_t p1, float& outMin, float& outMax) const;

    static constexpr int peakBinSize = 64;   // must match WaveformDisplay's own constant

private:
    void run() override;
    void kickOffJob(double speed, double pitch, double stretch);

    AudioDocument& document;
    static constexpr uint32_t debounceMs = 250;

    // Message-thread-only "delivered" state.
    double lastSeenSpeed = 1.0, lastSeenPitch = 0.0, lastSeenStretch = 1.0;
    int lastSeenBufferVersion = -1;
    uint32_t settleDeadlineMs = 0;
    bool waitingToSettle = false;
    std::vector<std::vector<float>> peakMin, peakMax;
    int64_t processedLength = 0;

    // Cross-thread handoff: the message thread overwrites the request (under `requestLock`)
    // and signals `wakeEvent`; the worker claims whatever's current when it wakes; if a newer
    // request already overwrote it by the time this one's done, it's simply superseded next
    // time the worker loops back round, rather than delivering an already-stale result. No
    // attempt to interrupt a RubberBand pass already in progress -- see the destructor.
    juce::WaitableEvent wakeEvent;
    juce::CriticalSection requestLock;
    bool haveRequest = false;
    juce::AudioBuffer<float> requestBuffer;
    double requestSpeed = 1.0, requestPitch = 0.0, requestStretch = 1.0, requestSampleRate = 44100.0;

    struct Result
    {
        std::vector<std::vector<float>> peakMin, peakMax;
        int64_t length = 0;
    };
    juce::CriticalSection resultLock;
    Result pendingResult;
    std::atomic<bool> resultReady { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformStretchPreview)
};
