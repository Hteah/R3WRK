#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

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

    //==============================================================================
    // Real-time pitch/tape engine, rebuilt in prepareToPlay().
    std::unique_ptr<RubberBand::RubberBandStretcher> rtStretcher;
    int rtChannels = 2;
    juce::AudioBuffer<float> rtScratchIn, rtScratchOut;
    bool wasPlaying = false;        // edge-detect play start -> reset the stretcher
    bool stretcherPrimed = false;   // stretcher holds state from the current play pass
    bool rtFinished = false;        // final block sent; only drain from here on

    static bool knobsEngaged(double speed, double pitch, double stretch);
    // Fills the playback branch of processBlock. `pos` is the doc read cursor (also the
    // value stored back into document.playhead).
    void renderPlaybackDirect (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                               const juce::AudioBuffer<float>& docBuf,
                               int64_t& pos, int64_t regionStart, int64_t regionEnd, bool loop);
    void renderPlaybackStretched (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                  const juce::AudioBuffer<float>& docBuf,
                                  int64_t& pos, int64_t regionStart, int64_t regionEnd, bool loop,
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
