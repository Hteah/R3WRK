#pragma once
#include <JuceHeader.h>

/**
    The in-memory audio buffer being edited, plus selection, playhead,
    loop points and undo history.

    Editing model: edits are snapshot-based. Call beginChange() before
    mutating getBufferForWriting(), do the mutation (resizing the buffer
    is fine), then call commitChange(name) to push an undo step.
*/
class AudioDocument
{
public:
    AudioDocument();

    //==============================================================================
    void newEmptyDocument(int numChannels, double sampleRate);

    // If resampleToRate is > 0 and differs from the file's own rate, the audio is
    // resampled on load so it plays back at the correct pitch/speed in the host.
    bool loadFromFile(const juce::File& file, double resampleToRate = 0.0);
    bool saveToFile(const juce::File& file) const;

    const juce::AudioBuffer<float>& getBuffer() const { return buffer; }

    double getSampleRate() const { return sampleRate; }
    void setSampleRate(double sr) { sampleRate = sr; }

    int64_t getNumSamples() const { return (int64_t) buffer.getNumSamples(); }
    int getNumChannels() const { return buffer.getNumChannels(); }
    bool isEmpty() const { return buffer.getNumSamples() == 0; }

    // Increments whenever the underlying audio content changes (not on selection/playhead
    // moves) -- lets expensive views (e.g. the spectrogram) know when to recompute.
    int getBufferVersion() const { return bufferVersion; }

    //==============================================================================
    // Selection, in samples, half-open [start, end). processBlock() (audio thread) reads
    // this to decide the playback region while the UI edits it, so start+end are packed
    // into one atomic (two separate atomics would let the audio thread read a new start
    // paired with a stale end mid-update -- e.g. while dragging the start of a looping
    // selection, a torn read could momentarily see a tiny/garbage region and buzz).
    void setSelection(int64_t start, int64_t end);
    juce::Range<int64_t> getSelection() const;
    int64_t getSelectionStart() const { return getSelection().getStart(); }
    int64_t getSelectionEnd() const { return getSelection().getEnd(); }
    bool hasSelection() const { const auto r = getSelection(); return r.getEnd() > r.getStart(); }
    void clearSelection() { setSelection(0, 0); }

    // returns the current selection, clamped to the buffer; if there is no
    // selection, returns the whole buffer.
    juce::Range<int64_t> getEffectiveRange() const;

    //==============================================================================
    std::atomic<int64_t> playhead { 0 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> isRecording { false };

    std::atomic<int64_t> loopStart { 0 };
    std::atomic<int64_t> loopEnd { 0 };
    std::atomic<bool> loopEnabled { false };

    //==============================================================================
    // Live playback knobs (KnobRow writes these; the audio thread reads them every block
    // to drive a real-time RubberBand stretcher -- see PluginProcessor). Live here, not on
    // the processor, so the views (WaveformDisplay, TimeRuler) can read them too, for the
    // visual time-stretch below.
    //   playbackSpeed   : tape-style rate. 1.0 = normal; 2.0 plays twice as fast AND an
    //                     octave up. Pitch rides along, exactly like tape.
    //   playbackPitch   : extra pitch shift in semitones, layered on top of the tape
    //                     speed, so you can detune without changing playback rate.
    //   playbackStretch : pure time-stretch, pitch preserved. 1.0 = off; 8.0 makes
    //                     playback eight times longer at the same pitch.
    std::atomic<double> playbackSpeed   { 1.0 };
    std::atomic<double> playbackPitch   { 0.0 };
    std::atomic<double> playbackStretch { 1.0 };

    static constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;
    static constexpr double kMinPitch = -12.0, kMaxPitch = 12.0;
    static constexpr double kMinStretch = 0.25, kMaxStretch = 50.0;

    // How much longer (>1) or shorter (<1) played-back audio is than stored audio, given
    // the current Speed/Pitch/Stretch knobs -- pitch doesn't affect duration, only the
    // other two do. WaveformDisplay uses this to visually stretch the waveform to match,
    // the same way most samplers show a slowed-down sample as visually longer.
    double getTimeScale() const
    {
        return playbackStretch.load(std::memory_order_relaxed)
             / juce::jmax(0.0001, playbackSpeed.load(std::memory_order_relaxed));
    }

    //==============================================================================
    // Live recording feedback, so the UI can show what's coming in before Stop.
    // processBlock (audio thread) fills a ring of block peak min/max while isRecording;
    // the waveform view reads it to draw a scrolling scope. Single producer / single
    // consumer -- a torn float read is cosmetically harmless.
    static constexpr int scopeSize = 1024;
    float scopeMin[scopeSize] = {};
    float scopeMax[scopeSize] = {};
    std::atomic<int> scopeWritePos { 0 };
    std::atomic<int64_t> recordedSamples { 0 };   // running length of the take
    void resetRecordingScope();

    //==============================================================================
    juce::UndoManager undoManager;

    // Snapshot-based change tracking. Callers copy getBuffer(), mutate the copy freely,
    // then pass it to commitChange() -- the live buffer is only ever swapped inside
    // restoreSnapshot(), under the lock, so the audio thread never sees a half-edited buffer.
    void beginChange();
    void commitChange(juce::AudioBuffer<float> newBuffer, const juce::String& actionName);

    // Broadcasts whenever the buffer, selection or markers change, so the UI can repaint.
    juce::ChangeBroadcaster changeBroadcaster;
    void notifyChanged() { changeBroadcaster.sendChangeMessage(); }

    //==============================================================================
    // Live, uncommitted preview of a pending Amplify or Stretch/Pitch edit -- set while the
    // corresponding pop-up panel's slider is being dragged (right-click a selection in
    // WaveformDisplay, or Tools ▾), cleared when the panel closes. Message-thread only (the
    // audio thread never reads these), so plain fields rather than atomics. EditActions and
    // the real buffer are untouched until "Apply" -- WaveformDisplay reads these only to draw
    // a live preview over the selection so an adjustment shows before it's committed.
    bool previewActive = false;
    float previewGainLinear = 1.0f;      // Amplify preview: linear gain for the selection's peaks
    double previewStretchRatio = 1.0;    // Stretch preview: the selection's visual width multiplier

    //==============================================================================
    // Internal: used by the undo action to swap buffer/selection state directly.
    void restoreSnapshot(const juce::AudioBuffer<float>& newBuffer, int64_t newSelStart, int64_t newSelEnd);

    //==============================================================================
    // The buffer can be resized/replaced by edits made on the message thread while the
    // audio thread is reading it for playback. Every point that mutates `buffer` (see
    // .cpp) takes this lock; processBlock() should take it too before reading the buffer
    // for playback. Edits are infrequent user actions, so a short critical section here
    // is a reasonable trade-off rather than a full lock-free double-buffer scheme.
    juce::CriticalSection& getLock() const { return bufferLock; }

private:
    mutable juce::CriticalSection bufferLock;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    std::atomic<uint64_t> selPacked { 0 };   // (uint32 start << 32) | uint32 end -- see getSelection()
    int bufferVersion = 0;

    juce::AudioBuffer<float> preChangeBuffer;
    int64_t preChangeSelStart = 0, preChangeSelEnd = 0;
    bool changeInProgress = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioDocument)
};
