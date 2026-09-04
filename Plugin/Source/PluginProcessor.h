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

    //==============================================================================
    // Live playback knobs (audio thread reads these every block).
    //   playbackSpeed : tape-style rate. 1.0 = normal; 2.0 plays twice as fast AND an
    //                   octave up. Pitch rides along, exactly like tape.
    //   playbackPitch : extra pitch shift in semitones, layered on top of the tape
    //                   speed, so you can detune without changing playback rate.
    // Both are realised by a real-time RubberBand stretcher on the playback stream; the
    // stored audio is never touched. When both are centred, playback bypasses RubberBand
    // entirely (zero latency / zero cost).
    std::atomic<double> playbackSpeed { 1.0 };
    std::atomic<double> playbackPitch { 0.0 };

    static constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;
    static constexpr double kMinPitch = -12.0, kMaxPitch = 12.0;

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

    static bool knobsEngaged(double speed, double pitch);
    // Fills the playback branch of processBlock. `pos` is the doc read cursor (also the
    // value stored back into document.playhead).
    void renderPlaybackDirect (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                               const juce::AudioBuffer<float>& docBuf,
                               int64_t& pos, int64_t regionStart, int64_t regionEnd, bool loop);
    void renderPlaybackStretched (juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                  const juce::AudioBuffer<float>& docBuf,
                                  int64_t& pos, int64_t regionStart, int64_t regionEnd, bool loop,
                                  double speed, double pitch);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessor)
};
