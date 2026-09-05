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

    // Plugins shouldn't open native windows of their own, so this is given `this` as its
    // parentComponent (per the class's own docs) rather than defaulting to the desktop --
    // it then stays invisible until the mouse hovers a component with a tooltip set, and
    // scales with the editor/DAW like any other child. Without this, setTooltip() calls
    // anywhere in the editor (e.g. EditorToolbar's icon buttons) are wired but silent.
    juce::TooltipWindow tooltipWindow { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessorEditor)
};
