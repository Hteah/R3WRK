#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "HeaderBar.h"
#include "EditorToolbar.h"
#include "WaveformDisplay.h"
#include "SpectrogramDisplay.h"

class EdisonCloneAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit EdisonCloneAudioProcessorEditor(EdisonCloneAudioProcessor&);
    ~EdisonCloneAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    EdisonCloneAudioProcessor& processorRef;

    HeaderBar header;
    WaveformDisplay waveformDisplay;
    SpectrogramDisplay spectrogramDisplay;
    EditorToolbar toolbar;
    bool showingSpectrogram = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EdisonCloneAudioProcessorEditor)
};
