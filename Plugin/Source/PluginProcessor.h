#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"

class EdisonCloneAudioProcessor : public juce::AudioProcessor
{
public:
    EdisonCloneAudioProcessor();
    ~EdisonCloneAudioProcessor() override;

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

private:
    double currentSampleRate = 44100.0;

    // Growable accumulation buffer used while isRecording is true; only touched by the
    // audio thread while recording, and merged into `document` (via beginChange/commitChange,
    // which is lock-protected) once recording stops.
    juce::AudioBuffer<float> recordingAccumulator;
    int64_t recordingWritePos = 0;
    void ensureRecordingCapacity(int numChannels, int64_t additionalSamples);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EdisonCloneAudioProcessor)
};
