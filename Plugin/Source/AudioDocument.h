#pragma once
#include <JuceHeader.h>

/**
    The in-memory audio buffer being edited, plus selection, playhead,
    loop points, chop markers and undo history.

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
    // Selection, in samples, half-open [selStart, selEnd)
    void setSelection(int64_t start, int64_t end);
    int64_t getSelectionStart() const { return selStart; }
    int64_t getSelectionEnd() const { return selEnd; }
    bool hasSelection() const { return selEnd > selStart; }
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
    // Chop-to-grid markers (sample positions), purely a view/export aid.
    std::vector<int64_t> chopMarkers;
    double chopBpm = 120.0;
    int chopDivision = 16; // e.g. 16 = 1/16 notes

    void recalculateChopMarkers();

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
    int64_t selStart = 0, selEnd = 0;
    int bufferVersion = 0;

    juce::AudioBuffer<float> preChangeBuffer;
    int64_t preChangeSelStart = 0, preChangeSelEnd = 0;
    bool changeInProgress = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioDocument)
};
