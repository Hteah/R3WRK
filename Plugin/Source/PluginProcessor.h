#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "DesktopAudioCapture.h"
#include "BiquadFilter.h"
#include "LfoModule.h"
#include "ReverbEngine.h"
#include "PlexiphonEngine.h"

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

    // Black Box follows whichever audio is actually meaningful each block: the live input while
    // it's genuinely passing through (idle, recording), or R3WRK's own rendered output once
    // R3WRK has taken over the buffer to play/loop/scrub -- which otherwise silently "mutes" the
    // input from Black Box's perspective. blackBoxInputScratch holds a snapshot of the raw input,
    // taken at the very top of processBlock before anything below can touch `buffer`, so it's
    // still available to append later for the input-passthrough branches even though those
    // branches don't rewrite `buffer` themselves. Sized (not reallocated) once per block; JUCE's
    // setSize with avoidReallocating=true is a no-op once big enough, matching seekOldTail's
    // pattern elsewhere in this file.
    juce::AudioBuffer<float> blackBoxInputScratch;

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

    // Dragging the Start/End knobs during playback moves the region under a live playhead;
    // whenever that leaves `pos` outside the new region it gets snapped back to regionStart,
    // which is an arbitrary splice point and pops. Ramp in from silence over a few ms right
    // after such a snap instead -- can't un-click the frame that already played, but this
    // covers the attack, which is the audible part. `declickLen` is the ramp's total length
    // (samples), `declickRemaining` counts down from it each block.
    int declickLen = 0;
    int declickRemaining = 0;

    // The declick ramp above only softens the *incoming* edge of a jump -- fine for a plain
    // region-jump reset (fresh Play, Undo, a new file...), where there's no coherent "old"
    // material to blend from anyway. A manual seek while playing (see declickRequested's
    // comment on AudioDocument) is different: the audio right where the old playhead left off
    // was still playing normally, and cutting it off abruptly is *itself* an audible click on
    // top of the incoming one, however well that one's softened. `seekCrossfadeActive` marks
    // that the current declick ramp should also crossfade in `seekOldTail` -- a short, ordinary
    // continuation of the material from wherever the playhead was a moment ago (captured once,
    // while the lock is held, at the instant the seek is detected), fading it out exactly as
    // the new position fades in. `lastKnownPlayhead` is what makes that possible at all: our
    // own record of where playback last was, updated at the end of every playing block, so it
    // still holds the *old* value at the moment a manual seek has already overwritten
    // `document.playhead` itself.
    bool seekCrossfadeActive = false;
    juce::AudioBuffer<float> seekOldTail;
    int64_t lastKnownPlayhead = 0;

    // While AudioDocument::selectionEdgeDragging is nonzero (a Start/End knob or waveform
    // bracket is being dragged), regionStart/regionEnd below are slewed toward their live
    // (fast-moving) values instead of being read straight through -- so the window that's
    // actually playing scans smoothly across the file instead of teleporting every block, and
    // the ordinary loop/gatherRegion machinery keeps genuinely playing it the whole time (real
    // audio, real pitch, real loop crossfade -- not a synthesized stand-in). `dragRegionStart/
    // End` are the smoothed (fractional) edges, carried across blocks; `dragRegionSeeded` is
    // false until the first dragging block seeds them from the live region, and gets cleared
    // again on release so the *next* drag starts fresh rather than resuming a stale slew.
    double dragRegionStart = 0.0;
    double dragRegionEnd   = 0.0;
    bool   dragRegionSeeded = false;

    // Catching a window up to a big/fast drag means reading through material that hasn't
    // played yet, and there's no way to do that without EITHER a discontinuity somewhere OR
    // genuinely playing through it at an accelerated rate. Every previous attempt at the first
    // option was really just a differently-shaped discontinuity: a hard reset to `regionStart`
    // (asymmetric -- zero margin against a forward-moving window, since reset always lands
    // right at its low edge, so a forward drag needed a fresh declick ramp nearly every block
    // that could never finish before the next one arrived -- a declick ramp that keeps re-
    // arming *is itself* an audible buzz, not a fix); then a carried jump by the same amount,
    // un-declicked (still a splice, just softer to reason about, not softer to hear); then that
    // same carried jump explicitly declicked (same problem as the reset: re-arms before
    // finishing). None of those were ever going to sound smooth, because they're all fixed-size
    // per-block teleports dressed up differently.
    //
    // renderDragScan() is the second option instead: a continuously-advancing fractional read
    // position (`dragScanPos`) that never jumps at all, so there's nothing to declick. When
    // behind the window it closes the gap at a speed proportional to the remaining distance
    // (capped, so a huge jump sounds like a bounded fast wind, not a shriek) -- a real, if
    // sped-up, read of the actual buffer, not a synthesized stand-in. Once inside
    // [regionStart, regionEnd) it settles to plain 1x forward and genuinely loops within it,
    // wrapping by subtracting the region length so the fractional position stays continuous
    // across the wrap too (with the same loopFadeGain crossfade gatherRegion itself uses, so
    // *that* doesn't click either). Plain (non-RubberBand) path only -- continuously perturbing
    // the read position under renderPlaybackStretched confused it into sustained distortion
    // when a discrete version of this was first tried (reverted as eb4ec70/9376798); the
    // RubberBand-engaged case keeps the ordinary hard-snap-and-declick path, unimproved but no
    // worse than it's always been. `dragScanActive` is false until the first dragging block
    // seeds `dragScanPos` from the live playhead, and again on release so the next drag (or a
    // handoff to the stretched path) starts fresh and the final position gets committed back to
    // `document.playhead`.
    double dragScanPos = 0.0;
    bool   dragScanActive = false;
    void renderDragScan(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                        const juce::AudioBuffer<float>& docBuf, double& pos,
                        int64_t regionStart, int64_t regionEnd, bool loop, int fadeLen);

    // Modelled Monomachine multimode filter on the playback output (after the stretcher). One
    // MultiModeFilter per channel; all four knob values are continuously smoothed (see
    // applyPlaybackFilter()) so a manual sweep doesn't zipper the coefficients. This is ONLY
    // used for manual, knob-driven filtering now -- an actively LFO-modulated filter runs
    // through applyModulatedFilter()/modulatedFilter instead (below): this direct-form biquad
    // isn't built to have its coefficients changed continuously and fast (proven non-finite
    // offline under that stress -- see Tests/SmokeTest.cpp), so it's kept for the case it's
    // actually good at instead of trying to make it handle both.
    r3wrk::MultiModeFilter playbackFilter[2];   // MnM Base/Width/HP Q/LP Q, per channel
    juce::SmoothedValue<double> smoothedFilterBase  { 0.0 };
    juce::SmoothedValue<double> smoothedFilterWidth { 1.0 };
    juce::SmoothedValue<double> smoothedFilterHpQ   { 0.0 };
    juce::SmoothedValue<double> smoothedFilterLpQ   { 0.0 };
    bool lastFilterEngaged = false;   // edge-detects disengaged->engaged, to clear stale filter memory (z1/z2) -- see applyPlaybackFilter()
    void applyPlaybackFilter (juce::AudioBuffer<float>& buffer, int numCh, int startSample, int numSamples,
                              bool freshPlayPass);

    // The LFO-modulated filter path: r3wrk::ModulatedMultiModeFilter (BiquadFilter.h) is a TPT
    // state-variable filter, built to tolerate its cutoff/resonance being recomputed every
    // sample -- unlike playbackFilter above. Reuses the exact same Base/Width/HP Q/LP Q -> Hz/Q
    // curve mapping, so a given position means the same cutoff/resonance either way; it's meant
    // to sound like the same filter, just modulation-stable. applyModulatedFilter() scans
    // document.lfoSlots itself each block: if nothing is actively targeting a filter parameter
    // it does nothing (returns false) and processBlock() falls back to the ordinary
    // playbackFilter path above; if something is, it handles the ENTIRE block itself --
    // ticking every filter-targeting LFO truly per-sample (not chunked -- see
    // kLfoModUpdateSamples's comment on why chunking doesn't work here), recomputing
    // coefficients every sample, and filtering -- so playbackFilter is skipped entirely for
    // that block (no double filtering).
    r3wrk::ModulatedMultiModeFilter modulatedFilter[2];
    bool modulatedFilterWasActive = false;   // edge-detects the switch into/out of this path, to reset cleanly
    bool applyModulatedFilter (juce::AudioBuffer<float>& buffer, int numCh, int numSamples, bool freshPlayPass);

    // LFO modulation: one r3wrk::LfoModule per AudioDocument::LfoSlot, holding the phase/RNG
    // state that's genuinely audio-thread-only (never read by the UI, so it doesn't need to
    // live on `document` the way the slots' user-facing config does). Two separate ticking
    // paths, so a slot's phase is only ever advanced by exactly one of them, never both:
    // tickLfos() (below) handles Gain-targeting slots, chunked (see kLfoModUpdateSamples);
    // applyModulatedFilter() handles filter-targeting slots itself, truly per-sample.
    r3wrk::LfoModule lfoDsp[AudioDocument::kMaxLfos];
    struct LfoModResult { double gainDb = 0.0; };
    LfoModResult tickLfos (int numSamples);
    // Gain has no natural 0..1 range the way the filter knobs do, so an Amount of 100% on a
    // Gain-targeted LFO is defined to swing +/- this many dB. Filter targets ARE already 0..1,
    // so applyModulatedFilter() just adds (LFO output * Amount) directly -- no dB scaling there.
    static constexpr double kLfoGainModRangeDb = 24.0;

    // LFO-driven Gain modulation is only as smooth as how often the gain ramp gets recomputed --
    // once per host block was fine when only a knob drag moved it (many small steps already, at
    // UI speed), but an LFO can swing the whole way across its range within a single large host
    // block, which sounded like a discrete "staircase" (audible as crackle) rather than a sweep.
    // processBlock() calls tickLfos()/applyPlaybackGain() once per kLfoModUpdateSamples-sized
    // sub-chunk instead of once for the whole block, so the update rate no longer depends on
    // whatever block size the host happens to use. (The filter path doesn't have this limit at
    // all -- applyModulatedFilter() runs truly per-sample.)
    //
    // This update rate is itself a sample rate as far as the LFO's own waveform is concerned --
    // an LFO faster than half of it (sampleRate / (2 * kLfoModUpdateSamples)) aliases: it folds
    // back down and reappears as a garbled, slower-sounding rate instead of a real fast one. 64
    // samples puts that ceiling around 350-375 Hz. Tried dropping this to 4 samples to chase
    // Gain's full "audio" range -- DON'T: recomputing the gain ramp 12,000+ times a second
    // turned out to be its own noise source. (This constant no longer affects the filter path,
    // which is unconditionally per-sample now, so this limitation is Gain-only.)
    static constexpr int kLfoModUpdateSamples = 64;

    // Output "Gain" knob -- applied last, as a per-block ramp so a knob drag doesn't zipper.
    // 0 dB by default, so a no-op unless the Standalone's Gain knob is turned (or an LFO targets it).
    juce::SmoothedValue<float> smoothedGain { 1.0f };
    void applyPlaybackGain (juce::AudioBuffer<float>& buffer, int numCh, int startSample, int numSamples,
                            const LfoModResult& lfoMod);

    // Erbe-Verb reverb (r3wrk::ErbeVerbReverb, ReverbEngine.h) -- phase 1: core engine only, live
    // monitoring, not baked into Save/Export. One instance (the algorithm is inherently stereo).
    // Params aren't LFO-modulated in phase 1, so applied once per whole block, unchunked -- unlike
    // the filter/gain above. reverbTailSamplesLeft counts down while playback is stopped so the
    // tail keeps ringing out on host passthrough audio, then stops -- see applyReverb()'s comment
    // for why an unconditional idle-block call would be a standing, surprising side effect on a
    // DAW channel that isn't even using R3WRK's own playback. It's seeded with a generous ceiling
    // (not a real prediction of tail length -- near-max Decay is *designed* for near-infinite
    // sustain, matching the real hardware) purely to bound worst-case background CPU if it's left
    // running unattended; reverbTailSilentSamples is what actually ends it once the tail is
    // genuinely inaudible, however long that takes for the current Decay setting.
    r3wrk::ErbeVerbReverb reverbDsp;
    juce::SmoothedValue<double> smoothedReverbSize     { 0.5 };
    juce::SmoothedValue<double> smoothedReverbAbsorb   { 0.5 };
    juce::SmoothedValue<double> smoothedReverbDecay    { 0.5 };
    juce::SmoothedValue<double> smoothedReverbTilt     { 0.5 };
    juce::SmoothedValue<double> smoothedReverbMix      { 0.0 };
    juce::SmoothedValue<double> smoothedReverbPredelay { 0.1 };
    juce::SmoothedValue<double> smoothedReverbWidth    { 0.5 };
    int  reverbTailSamplesLeft   = 0;
    int  reverbTailSilentSamples = 0;
    bool lastReverbEngaged = false;
    // tailOnly: false (the playing path) feeds the buffer's real content in and replaces it with
    // the dry/wet mix, same as the filter/gain do. true (the idle tail-ring-out path) feeds
    // silence in instead -- so only whatever's still recirculating in the delay lines comes back
    // out -- and ADDS that to the buffer rather than replacing it, since idle-block `buffer` is
    // live host passthrough (or silence) that must stay intact underneath the decaying tail.
    // Returns the peak absolute sample value the reverb itself produced this call -- meaningful
    // (and used by the caller to track reverbTailSilentSamples) only for tailOnly calls; the
    // live path's caller ignores it.
    float applyReverb (juce::AudioBuffer<float>& buffer, int numCh, int numSamples, bool freshPlayPass,
                       bool tailOnly = false);

    // Plexiphon (r3wrk::PlexiphonEngine, PlexiphonEngine.h) -- phase 1: mono core network, live
    // monitoring, not baked into Save/Export. Exact structural mirror of the Reverb wiring
    // above, including the lesson learned building it: does NOT reset plexDsp on freshPlayPass
    // (see applyPlexiphon()'s comment) -- only ErbeVerbReverb ever had that bug, caught live
    // after shipping; built in correctly here from the start instead.
    r3wrk::PlexiphonEngine plexDsp;
    juce::SmoothedValue<double> smoothedPlexLevel   { 0.5 };
    juce::SmoothedValue<double> smoothedPlexPlexus  { 0.5 };
    juce::SmoothedValue<double> smoothedPlexSize    { 0.5 };
    juce::SmoothedValue<double> smoothedPlexDiffuse { 0.5 };
    juce::SmoothedValue<double> smoothedPlexDecay   { 0.5 };
    juce::SmoothedValue<double> smoothedPlexColor   { 0.5 };
    juce::SmoothedValue<double> smoothedPlexMix     { 0.0 };
    int  plexTailSamplesLeft   = 0;
    int  plexTailSilentSamples = 0;
    bool lastPlexEngaged = false;
    float applyPlexiphon (juce::AudioBuffer<float>& buffer, int numCh, int numSamples, bool freshPlayPass,
                         bool tailOnly = false);

    static bool knobsEngaged(double speed, double pitch, double stretch);
    // Fills the playback branch of processBlock. `pos` is the doc read cursor (also the
    // value stored back into document.playhead).
    void renderPlaybackDirect (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                               const juce::AudioBuffer<float>& docBuf,
                               int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                               bool loop, bool pingPong, bool reverseLoop, int loopFadeLen);
    void renderPlaybackStretched (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                  const juce::AudioBuffer<float>& docBuf,
                                  int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                                  bool loop, bool pingPong, bool reverseLoop, int loopFadeLen,
                                  double speed, double pitch, double stretch);

    // Scrub tool: a plain variable-rate (and reversible) read of the stored audio, driven by
    // AudioDocument::scrubVelocity -- see the class comment there. Deliberately *not* run
    // through RubberBand: pitch tracking speed/direction one-to-one is the point (a real
    // tape or turntable does the same), not a separate DSP mode to maintain.
    double scrubReadPos = 0.0;     // audio-thread-only fractional read cursor, raw samples
    bool wasScrubbing = false;     // edge-detect scrub start, so scrubReadPos picks up from
                                   // document.playhead (wherever the drag began) rather than
                                   // continuing from a stale previous position

    // Releasing the mouse mid-scrub used to stop dead -- the scrub branch returns before ever
    // reaching the shared declickRemaining ramp below, so a real-speed scrub just cut off
    // whatever amplitude it happened to be at. On document.scrubStopRequested, fade scrub's own
    // output to silence over scrubStopFadeLen samples (same 8ms/equal-power shape as the region-
    // jump declick, just applied here since the scrub branch can't reach that code), then clear
    // isScrubbing/scrubVelocity itself once the fade completes.
    int scrubStopFadeLen = 0;
    int scrubStopFadeRemaining = 0;
    void renderScrub(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                     const juce::AudioBuffer<float>& docBuf);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessor)
};
