#pragma once
#include <JuceHeader.h>

/**
    The in-memory audio buffer being edited, plus selection, playhead,
    loop points and undo history.

    Editing model: edits are snapshot-based. Call beginChange() before
    mutating getBufferForWriting(), do the mutation (resizing the buffer
    is fine), then call commitChange(name) to push an undo step -- each
    commitChange() call is its own separate undo step (it opens a fresh
    juce::UndoManager transaction itself), never merged with the edit before
    or after it.
*/

// Chosen in the "Save Options" panel, persisted by OutputSettings, and passed to
// AudioDocument::saveToFile(). `sampleRate == 0` means "keep the document's own rate";
// `bitDepth == 32` means 32-bit float (WAV only -- clamped to 24 for AIFF/FLAC, ignored for
// MP3, which is always 16-bit CBR/VBR through the encoder).
struct AudioSaveOptions
{
    enum class Format { wav, aiff, flac, mp3 };

    Format format     = Format::wav;
    int    sampleRate = 0;
    int    bitDepth   = 24;

    juce::String extension() const
    {
        switch (format)
        {
            case Format::aiff: return ".aiff";
            case Format::flac: return ".flac";
            case Format::mp3:  return ".mp3";
            case Format::wav:
            default:           return ".wav";
        }
    }

    juce::String formatName() const
    {
        switch (format)
        {
            case Format::aiff: return "AIFF";
            case Format::flac: return "FLAC";
            case Format::mp3:  return "MP3";
            case Format::wav:
            default:           return "WAV";
        }
    }

    // "WAV · 48 kHz · 24-bit" for the Tools-menu item text.
    juce::String shortSummary() const
    {
        juce::String rate = sampleRate <= 0 ? juce::String ("keep rate")
                                            : juce::String (sampleRate / 1000.0, sampleRate % 1000 == 0 ? 0 : 1) + " kHz";
        juce::String depth = format == Format::mp3 ? juce::String ("VBR")
                           : bitDepth == 32        ? juce::String ("32-bit float")
                                                   : juce::String (bitDepth) + "-bit";
        return formatName() + juce::String::fromUTF8 (" \xc2\xb7 ") + rate + juce::String::fromUTF8 (" \xc2\xb7 ") + depth;
    }

    static Format formatFromName (const juce::String& n)
    {
        if (n.equalsIgnoreCase ("AIFF")) return Format::aiff;
        if (n.equalsIgnoreCase ("FLAC")) return Format::flac;
        if (n.equalsIgnoreCase ("MP3"))  return Format::mp3;
        return Format::wav;
    }
};

class AudioDocument
{
public:
    AudioDocument();

    //==============================================================================
    void newEmptyDocument(int numChannels, double sampleRate);

    // If resampleToRate is > 0 and differs from the file's own rate, the audio is
    // resampled on load so it plays back at the correct pitch/speed in the host.
    bool loadFromFile(const juce::File& file, double resampleToRate = 0.0);

    // Writes the stored audio to `file`. The Speed/Pitch/Stretch knobs are always baked into
    // the written audio (renderWithPlaybackKnobs -- a no-op copy when they're centred), so the
    // file matches what you hear; the in-memory document and the knobs are left untouched.
    // The `opts` overload also picks the container format, resamples to opts.sampleRate (0 =
    // keep), and writes at opts.bitDepth; the bare overload is WAV / keep-rate / 24-bit.
    bool saveToFile(const juce::File& file) const;
    bool saveToFile(const juce::File& file, const AudioSaveOptions& opts) const;

    // Writes `src` (at `srcRate`, knob-baked by the caller if desired) to `file` in the
    // container / sample rate / bit depth from `opts`. Shared by saveToFile() and
    // EditActions::exportSelection() so Save and Export write identically.
    static bool writeAudioFile(juce::AudioBuffer<float> src, double srcRate,
                               const juce::File& file, const AudioSaveOptions& opts);

    // MP3 export shells out to a LAME binary (JUCE has no built-in MP3 encoder). These say
    // whether one was found on this machine, so the UI can enable/disable the MP3 option.
    static juce::File findLameBinary();
    static bool mp3ExportAvailable() { return findLameBinary().existsAsFile(); }

    // Lagrange resample every channel from srcRate to dstRate (returns `src` unchanged when
    // the rates already match). Used by saveToFile() and the fixed-rate Octatrack export.
    static juce::AudioBuffer<float> resampled(const juce::AudioBuffer<float>& src,
                                              double srcRate, double dstRate);

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
    // Slice markers: sample positions used by Tools ▾ -> "Export Slices" / "Export Octatrack
    // Chain". Placed/edited via the Slice tool (EditorToolbar's Slice toggle -> sliceModeEnabled;
    // in that mode WaveformDisplay repurposes the mouse: double-left-click = add, right-click =
    // play the slice, drag-a-marker = move, double-left-click a marker's top/bottom handle = delete).
    // Message-thread only (the audio thread doesn't touch them -- "play slice" just sets the
    // selection + normal playback), always kept sorted + de-duplicated + strictly inside
    // (0, getNumSamples()). NOT persisted (session-only) and NOT part of the undo snapshot --
    // and any edit that changes the sample length
    // (trim/cut/stretch/...) clears them, since their absolute positions would no longer line
    // up with the audio (see restoreSnapshot()).
    bool sliceModeEnabled = false;   // the Slice *tool* being selected -- message-thread only

    void addSliceMarker (int64_t sample);
    void removeSliceMarker (int index);                       // no-op if out of range
    int  moveSliceMarker (int index, int64_t newSample);      // returns the marker's index after re-sorting
                                                              // (the surviving one if it merged onto another)
    void clearSliceMarkers();
    const std::vector<int64_t>& getSliceMarkers() const { return sliceMarkers; }
    int findSliceMarkerNear (int64_t sample, int64_t tolerance) const;   // nearest within tolerance, else -1

    // The regions between markers: [0, m0), [m0, m1), ..., [mLast, len). Empty when there are
    // no markers (nothing to slice). k markers -> k+1 regions.
    std::vector<juce::Range<int64_t>> getSliceRegions() const;

    //==============================================================================
    // Channel focus: which channel(s) the gain-shaped region processors work on. Message-
    // thread-only editing state -- the audio thread never reads it (playback and monitoring
    // always use every channel). Amplify (+ its live preview), Normalize, Gain, Fade In / Fade
    // Out, Reverse and Silence honour it; structural edits (cut/copy/paste/trim/delete/
    // stretch) don't, since a stereo clip can't have channels of different lengths. Reset to
    // Stereo on load / new / clear / record.
    enum class ChannelFocus { stereo, left, right };
    ChannelFocus channelFocus = ChannelFocus::stereo;

    bool channelInFocus (int ch) const
    {
        return channelFocus == ChannelFocus::stereo
            || (channelFocus == ChannelFocus::left  && ch == 0)
            || (channelFocus == ChannelFocus::right && ch == 1);
    }

    //==============================================================================
    std::atomic<int64_t> playhead { 0 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> isRecording { false };

    std::atomic<int64_t> loopStart { 0 };
    std::atomic<int64_t> loopEnd { 0 };
    std::atomic<bool> loopEnabled { false };

    //==============================================================================
    // Scrub tool: drag across the waveform to play forward or backward at a rate matching
    // how fast you drag -- like moving tape past a playback head by hand, or a turntable
    // under a stylus. Pitch rises and falls with speed (this is a plain variable-rate read,
    // no RubberBand correction), which is the whole point -- that's the "tape speeding up
    // and slowing down" sound, not a clean speed change.
    //   scrubModeEnabled : the *tool* being selected (EditorToolbar's Scrub toggle) --
    //                      message-thread only, like previewActive, so a plain bool.
    //   isScrubbing      : true only while actually dragging; the audio thread reads this
    //                      every block, so atomic.
    //   scrubVelocity    : current rate in raw samples per second, signed (negative =
    //                      backward), written by WaveformDisplay on every drag move,
    //                      read by the audio thread each block.
    bool scrubModeEnabled = false;
    std::atomic<bool> isScrubbing { false };
    std::atomic<double> scrubVelocity { 0.0 };

    //==============================================================================
    // Follow-playhead: while playing and zoomed in, WaveformDisplay slides its view each
    // frame to keep the playhead centred (the waveform scrolls under a fixed cursor).
    // EditorToolbar's Follow toggle -- message-thread only, session-only (not persisted),
    // like scrubModeEnabled / sliceModeEnabled.
    bool followPlayheadEnabled = false;

    //==============================================================================
    // Auto-Record: arm this and R3WRK starts recording for real the moment the input level
    // crosses autoRecordThresholdDb -- a level-triggered record standby (a tape deck's
    // voice-activated record), not the plain Record button's "start right now".
    //   autoRecordEnabled     : the toggle being armed (EditorToolbar's Auto-Record button) --
    //                           read every idle block by the audio thread, so atomic.
    //   autoRecordThresholdDb : the peak level (dBFS) that ends the standby and starts
    //                           recording; written by the threshold panel (Tools ▾ menu),
    //                           read by the audio thread. Persisted in plugin state, like the
    //                           knob-row Speed/Pitch/Stretch, since it's a setting worth
    //                           remembering across sessions, not a one-off toggle.
    //   autoRecordTriggered   : the audio thread sets this the instant the threshold is
    //                           crossed -- it never calls startRecording() itself (that
    //                           allocates memory and drives document/recording state meant to
    //                           come from the message thread); EditorToolbar's 15Hz timer
    //                           notices it and does the actual start, the same "audio thread
    //                           only flips an atomic, message thread acts on it" pattern every
    //                           other UI-facing handoff in this class already follows.
    std::atomic<bool> autoRecordEnabled { false };
    std::atomic<double> autoRecordThresholdDb { -40.0 };
    std::atomic<bool> autoRecordTriggered { false };

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

    // Multi-mode filter on the playback stream (after the stretcher). Also non-destructive and
    // baked into Save/Export by renderWithPlaybackKnobs. See BiquadFilter.h.
    //   filterMode      : 0 off, 1 low-pass, 2 high-pass, 3 band-pass, 4 notch (KnobRow's
    //                     stepped "Filter" knob).
    //   filterCutoffHz  : corner frequency, 20..20000 Hz ("Cutoff" knob, log-skewed).
    //   filterResonance : 0..1, mapped to Q by r3wrk::filterResonanceToQ ("Res" knob).
    std::atomic<int>    filterMode      { 0 };
    std::atomic<double> filterCutoffHz  { 1000.0 };
    std::atomic<double> filterResonance { 0.0 };

    // Output "Gain" knob (Standalone only -- KnobRow adds the knob just in that build; the
    // atomic sits here for everyone but stays at 0). In dB, 0 = unity (default volume);
    // kMinGainDb reads as a true mute. Applied last on the playback/scrub output and baked
    // into Save/Export by renderWithPlaybackKnobs, same as the other playback knobs.
    std::atomic<double> playbackGainDb { 0.0 };

    static constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;
    static constexpr double kMinPitch = -12.0, kMaxPitch = 12.0;
    static constexpr double kMinStretch = 0.25, kMaxStretch = 50.0;
    static constexpr double kFilterMinHz = 20.0, kFilterMaxHz = 20000.0;
    static constexpr double kMinGainDb = -60.0, kMaxGainDb = 12.0;

    // How much longer (>1) or shorter (<1) played-back audio is than stored audio, given
    // the current Speed/Pitch/Stretch knobs -- pitch doesn't affect duration, only the
    // other two do. WaveformDisplay uses this to visually stretch the waveform to match,
    // the same way most samplers show a slowed-down sample as visually longer.
    double getTimeScale() const
    {
        return playbackStretch.load(std::memory_order_relaxed)
             / juce::jmax(0.0001, playbackSpeed.load(std::memory_order_relaxed));
    }

    // True when the Speed/Pitch/Stretch knobs are not all at identity (centre).
    bool timePitchKnobsEngaged() const;

    // True when ANY playback knob (time/pitch/stretch OR the filter) would change the sound.
    bool playbackKnobsEngaged() const;

    // Renders `src` through the current playback knobs so a written file captures the sound,
    // not the knob positions:
    //   time/pitch/stretch  -- the OFFLINE stretch engine, same mapping the real-time path
    //                          uses: timeRatio = stretch/speed, semitones = 12*log2(speed)+pitch
    //   filter              -- the same r3wrk::Biquad the real-time path uses, run over the
    //                          (already stretched) buffer
    // Returns a plain copy of `src` when every knob is at identity, or if the stretch engine
    // fails. Used by Save As / auto-save / Export Selection / slice export.
    juce::AudioBuffer<float> renderWithPlaybackKnobs(const juce::AudioBuffer<float>& src) const;

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
    // "Revert to Original" (Tools ▾) -- undoes *editing* done to a loaded or recorded sample,
    // without touching the load/record itself. Replaces an earlier "Revert" that just walked
    // the undo stack (undoManager.undo() in a loop) all the way to empty -- fine for a loaded
    // file (loadFromFile() clears undo history, so there was nothing to walk back through) but
    // wrong for a recording, where stopRecording()'s own commitChange("Record") was itself just
    // one more undoable step: reverting a fresh take walked straight past it and erased the
    // recording. originalBuffer is a separate snapshot, untouched by ordinary edits (Trim,
    // Amplify, Paste, ...) -- markAsOriginal() re-takes it whenever a *new* load/record/session
    // establishes a genuinely new starting point; revertToOriginal() restores it the same way
    // any other edit commits (an ordinary, undoable/redoable SnapshotAction), so it can never
    // reach back further than the original take, and reverting is itself undoable if it wasn't
    // what you meant to do.
    void markAsOriginal();
    void revertToOriginal();

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

    std::vector<int64_t> sliceMarkers;   // see the slice-marker section above
    void normaliseSliceMarkers();        // sort + dedupe + drop anything not strictly inside (0, len)

    juce::AudioBuffer<float> originalBuffer;   // see markAsOriginal()/revertToOriginal() above

    juce::AudioBuffer<float> preChangeBuffer;
    int64_t preChangeSelStart = 0, preChangeSelEnd = 0;
    bool changeInProgress = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioDocument)
};
