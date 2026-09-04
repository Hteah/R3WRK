#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "HeaderBar.h"
#include "EditorToolbar.h"
#include "KnobRow.h"
#include "WaveformDisplay.h"
#include "SpectrogramDisplay.h"
#include "TimeRuler.h"
#include "Theme.h"

class R3WRKAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::ChangeListener
{
public:
    explicit R3WRKAudioProcessorEditor(R3WRKAudioProcessor&);
    ~R3WRKAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    R3WRKAudioProcessor& processorRef;
    juce::SharedResourcePointer<ThemeManager> theme;

    HeaderBar header;
    WaveformDisplay waveformDisplay;
    SpectrogramDisplay spectrogramDisplay;
    TimeRuler timeRuler;
    EditorToolbar toolbar;
    KnobRow knobRow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessorEditor)
};
