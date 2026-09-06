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
                                  private juce::ChangeListener,
                                  public juce::FileDragAndDropTarget
{
public:
    explicit R3WRKAudioProcessorEditor(R3WRKAudioProcessor&);
    ~R3WRKAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override;   // drag the window by the top inset (Standalone/macOS)

    // Drag a sample in from Finder (or a DAW's browser) and drop it anywhere on the window to
    // load it, same as Tools ▾ -> "Open…" -- covers the whole editor rather than just the
    // waveform, since none of the child components (WaveformDisplay included) implement
    // FileDragAndDropTarget themselves, so JUCE's peer keeps walking up the component
    // hierarchy from whatever's under the cursor until it reaches this one.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    bool showingDropHighlight = false;

    // In the Standalone build the macOS traffic lights float over the top-left of our own UI
    // (native title bar, no strip -- see StandaloneWindowShape.mm), so reserve a thin band at
    // the top for them. Zero in a plugin (the DAW owns the window chrome).
    static constexpr int kMacTrafficLightInset = 22;
    const bool standaloneWindow;

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
