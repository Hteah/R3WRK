#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "DesktopAudioCapture.h"
#include "BiquadFilter.h"

namespace RubberBand { class RubberBandStretcher; }

class R3WRKAudioProcessor : public juce::AudioProcessor
{
public:
    R3WRKAudioProcessor();
    ~R3WRKAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    AudioDocument document;

    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();

    // "Record Desktop" (Standalone only): captures the Mac's system audio via ScreenCaptureKit
    // straight into the editor buffer. document.isRecording drives the same UI (scope, stop
    // square); isDesktopRecording() disambiguates so the editor greys the right buttons.
    // startDesktopRecording is async -- document.isRecording flips true on the stream's own
    // "started" callback (after the Screen Recording prompt); onDesktopStatus surfaces errors.
    void startDesktopRecording();
    void stopDesktopRecording();
    bool isDesktopRecording() const { return desktopRecording.load(std::memory_order_relaxed); }
    static bool isDesktopCaptureSupported() { return DesktopAudioCapture::isSupported(); }
    std::function<void(juce::String)> onDesktopStatus;

    // "Capture Output" (Standalone only): taps the fully-processed playback/scrub output --
    // stretch, pitch, filter, gain, everything you hear -- into a growable buffer while
    // capturing, then writes it to `dest` (format/bit-depth from `opts`) on stop. Does NOT
    // touch the document or the transport: you play/loop/twiddle knobs and it records the
    // performance. Returns the written file (extension coerced to the format), or File() on
    // failure / nothing captured.
    void startOutputCapture();
    juce::File stopOutputCaptureAndWrite(const juce::File& dest, const AudioSaveOptions& opts);
    bool isCapturingOutput() const { return capturingOutput.load(std::memory_order_relaxed); }

    // "Black Box" (VST/AU only): a ring buffer that always records this track's raw input in
    // the background, no arming needed -- see appendToBlackBox(), called at the very top of
    // processBlock(). isBlackBoxAvailable() is false in the Standalone app (it already has
    // Record Desktop / Capture Output as its own explicit safety nets, and there's no "track
    // input" to a Standalone instance the way there is to a plugin insert).
    // getBlackBoxSnapshot() hands the popup a chronological (oldest-first) copy of whatever's
    // currently in the ring, up to the full getBlackBoxDurationSecs() once it has wrapped at
    // least once; safe to call from the message thread.
    bool isBlackBoxAvailable() const { return blackBoxCapacity > 0; }
    juce::AudioBuffer<float> getBlackBoxSnapshot(double& sampleRateOut) const;

    // Ring length: 90 seconds or 5 minutes (kBlackBoxDurationShort/Long -- see
    // EditorToolbar's BlackBoxDurationPanel, the only place that offers a choice between
    // them), persisted in OutputSettings and read into a fresh instance's default at
    // construction. setBlackBoxDurationSecs() reallocates the ring immediately (dropping
    // whatever it currently holds -- same trade-off a sample-rate change already makes, see
    // prepareToPlay()), so a change from the popup takes effect on this track right away;
    // other already-loaded instances pick it up next time they're prepared, not live.
    static constexpr double kBlackBoxDurationShort = 90.0;
    static constexpr double kBlackBoxDurationLong  = 300.0;
    double getBlackBoxDurationSecs() const { return blackBoxDurationSecs; }
    void setBlackBoxDurationSecs(double secs);

    // Auditioning in the Black Box popup: whenever a selection commits there, it starts
    // looping that range straight out this track's output -- the same trade-off Record
    // Desktop / Capture Output already make, taking over processBlock() entirely (see
    // renderBlackBoxPreview()'s branch), so the track's normal output is muted for as long as
    // it loops. Resamples to the current rate first if `sourceRate` differs. Loops forever
    // until stopBlackBoxPreview() (clearing the selection, or the popup closing); the popup
    // polls isBlackBoxPreviewPlaying()/getBlackBoxPreviewPosition() to keep its waveform's
    // playhead following along.
    void startBlackBoxPreview(juce::AudioBuffer<float> audio, double sourceRate);
    void stopBlackBoxPreview();
    bool isBlackBoxPreviewPlaying() const { return blackBoxPreviewPlaying.load(std::memory_order_relaxed); }
    int64_t getBlackBoxPreviewPosition() const { return blackBoxPreviewPos.load(std::memory_order_relaxed); }

    // Live playback knobs (Speed/Pitch/Stretch) live on `document` now -- see
    // AudioDocument.h -- so the views can read them too, not just the audio thread. All
    // three are realised by a real-time RubberBand stretcher on the playback stream; the
    // stored audio is never touched. When all three are centred, playback bypasses
    // RubberBand entirely (zero latency / zero cost).

private:
    double currentSampleRate = 44100.0;

    // Growable accumulation buffer used while isRecording is true; only touched by the
    // audio thread while recording, and merged into `document` (via beginChange/commitChange,
    // which is lock-protected) once recording stops.
    juce::AudioBuffer<float> recordingAccumulator;
    int64_t recordingWritePos = 0;
    void ensureRecordingCapacity(int numChannels, int64_t additionalSamples);

    // Desktop capture: fed from ScreenCaptureKit's background queue (not the audio thread), so
    // a plain CriticalSection guards the growable buffer -- processBlock never touches it.
    DesktopAudioCapture desktopCapture;
    std::atomic<bool> desktopRecording { false };
    juce::CriticalSection desktopRecLock;
    juce::AudioBuffer<float> desktopRecBuffer;
    int64_t desktopRecWritePos = 0;
    double  desktopRecRate = 48000.0;
    int     desktopRecChannels = 2;
    void appendDesktopSamples(const float* const* data, int numChannels, int numFrames, double sr);
    void finalizeDesktopRecording();

    // Output capture: audio-thread-only growable buffer (same single-producer pattern as
    // recordingAccumulator). captureOutput() appends the finished output block while
    // capturingOutput; stopOutputCaptureAndWrite() clears the flag, then reads it.
    std::atomic<bool> capturingOutput { false };
    juce::AudioBuffer<float> outputCaptureBuffer;
    int64_t outputCaptureWritePos = 0;
    void captureOutput(const juce::AudioBuffer<float>& out, int numCh, int numSamples);

    // Black Box: audio-thread-written, message-thread-read ring buffer. blackBoxLock guards
    // blackBoxBuffer against the audio thread's writer -- appendToBlackBox() only ever
    // tryEnter()s it, so the audio thread never blocks; on the rare contended call (the UI
    // taking a snapshot) it just drops that one block rather than risk a priority inversion.
    // blackBoxCapacity is 0 in the Standalone app (allocation skipped in reallocateBlackBoxBuffer()),
    // which doubles as isBlackBoxAvailable()'s flag.
    double blackBoxDurationSecs = kBlackBoxDurationLong;   // see setBlackBoxDurationSecs()
    mutable juce::CriticalSection blackBoxLock;   // mutable: getBlackBoxSnapshot() locks it from a const method
    juce::AudioBuffer<float> blackBoxBuffer;
    std::atomic<int64_t> blackBoxWritePos { 0 };   // total samples ever written; physical index
                                                    // is this value mod blackBoxCapacity
    int blackBoxCapacity = 0;                      // samples per channel; 0 = disabled
    void reallocateBlackBoxBuffer();   // (re)sizes blackBoxBuffer for blackBoxDurationSecs at
                                       // currentSampleRate -- called from prepareToPlay() and
                                       // setBlackBoxDurationSecs()
    void appendToBlackBox(const juce::AudioBuffer<float>& buffer, int numCh, int numSamples);

    // Black Box preview playback (see startBlackBoxPreview()). Also guarded by blackBoxLock --
    // start/stop happen on the message thread while renderBlackBoxPreview() reads it on the
    // audio thread each block.
    juce::AudioBuffer<float> blackBoxPreviewBuffer;
    std::atomic<int64_t> blackBoxPreviewPos { 0 };
    std::atomic<bool> blackBoxPreviewPlaying { false };
    void renderBlackBoxPreview(juce::AudioBuffer<float>& out, int numCh, int numSamples);

    //==============================================================================
    // Real-time pitch/tape engine, rebuilt in prepareToPlay().
    std::unique_ptr<RubberBand::RubberBandStretcher> rtStretcher;
    int rtChannels = 2;
    juce::AudioBuffer<float> rtScratchIn, rtScratchOut;
    bool wasPlaying = false;        // edge-detect play start -> reset the stretcher
    bool stretcherPrimed = false;   // stretcher holds state from the current play pass
    bool rtFinished = false;        // final block sent; only drain from here on
    int  playbackDir = 1;           // +1 forward, -1 backward (ping-pong loop only); reset to
                                    // +1 at the start of every play pass

    // Turning a knob during playback used to feed RubberBand a hard staircase of time-ratio /
    // pitch-scale steps (one per block), which it renders as zipper noise / pops. Ramp both
    // toward their target over ~120 ms instead -- multiplicative so equal ratio changes glide
    // equally -- sampled once per block. `stretchRatioNeedsSnap` jumps straight to target on a
    // fresh play pass / bypass<->engage flip so playback doesn't start with a 120 ms slide.
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> smoothedTimeRatio  { 1.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> smoothedPitchScale { 1.0 };
    double lastAppliedTimeRatio  = -1.0;
    double lastAppliedPitchScale = -1.0;
    bool   stretchRatioNeedsSnap = true;

    // Modelled Monomachine multimode filter on the playback output (after the stretcher). One
    // MultiModeFilter per channel; all four knob values are per-block-smoothed so a sweep
    // doesn't zipper the coefficients. Reset on a fresh play pass and when the engaged line is
    // crossed, so re-enabling doesn't thump.
    r3wrk::MultiModeFilter playbackFilter[2];   // MnM Base/Width/HP Q/LP Q, per channel
    juce::SmoothedValue<double> smoothedFilterBase  { 0.0 };
    juce::SmoothedValue<double> smoothedFilterWidth { 1.0 };
    juce::SmoothedValue<double> smoothedFilterHpQ   { 0.0 };
    juce::SmoothedValue<double> smoothedFilterLpQ   { 0.0 };
    bool lastFilterEngaged = false;
    void applyPlaybackFilter (juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                              bool freshPlayPass);

    // Output "Gain" knob -- applied last, as a per-block ramp so a knob drag doesn't zipper.
    // 0 dB by default, so a no-op unless the Standalone's Gain knob is turned.
    juce::SmoothedValue<float> smoothedGain { 1.0f };
    void applyPlaybackGain (juce::AudioBuffer<float>& buffer, int numCh, int numSamples);

    static bool knobsEngaged(double speed, double pitch, double stretch);
    // Fills the playback branch of processBlock. `pos` is the doc read cursor (also the
    // value stored back into document.playhead).
    void renderPlaybackDirect (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                               const juce::AudioBuffer<float>& docBuf,
                               int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                               bool loop, bool pingPong, int loopFadeLen);
    void renderPlaybackStretched (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                  const juce::AudioBuffer<float>& docBuf,
                                  int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                                  bool loop, bool pingPong, int loopFadeLen,
                                  double speed, double pitch, double stretch);

    // Scrub tool: a plain variable-rate (and reversible) read of the stored audio, driven by
    // AudioDocument::scrubVelocity -- see the class comment there. Deliberately *not* run
    // through RubberBand: pitch tracking speed/direction one-to-one is the point (a real
    // tape or turntable does the same), not a separate DSP mode to maintain.
    double scrubReadPos = 0.0;     // audio-thread-only fractional read cursor, raw samples
    bool wasScrubbing = false;     // edge-detect scrub start, so scrubReadPos picks up from
                                   // document.playhead (wherever the drag began) rather than
                                   // continuing from a stale previous position
    void renderScrub(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                     const juce::AudioBuffer<float>& docBuf);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessor)
};
