#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "EditActions.h"
#include "PluginProcessor.h"

/**
    A single control strip below the waveform:

        Play · Loop · time    ···    ● REC m:ss · Record · Tools ▾

    Everything else — file ops, clipboard, region processing, stretch/pitch,
    chop-to-grid, undo/redo — lives in the "Tools" pop-up menu.
    Owns the clipboard and talks to the AudioDocument / processor directly.
*/
class EditorToolbar : public juce::Component,
                      public juce::ChangeListener,
                      private juce::Timer
{
public:
    EditorToolbar(EdisonCloneAudioProcessor& processor, AudioDocument& document);
    ~EditorToolbar() override;

    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    std::function<void(juce::String name)> onSourceNameChanged;   // "" for a fresh recording
    std::function<void()> onSaved;                                // after a successful open / save

    // Called by the editor's keyPressed so ⌘X/C/V/Z and Space work from anywhere.
    void doCut();
    void doCopy();
    void doPaste();
    void doUndo();
    void doRedo();
    void toggleTransport();   // Record button: record if idle, else stop
    void togglePlay();        // Space: play if idle, else stop

private:
    void timerCallback() override;
    void updateTransportButtonText();
    void openFile();
    void saveFile();
    void revertAll();
    void showToolsMenu();
    void showAmplifyCallout();
    void showStretchCallout();
    void showChopCallout();
    void exportSlices();

    EdisonCloneAudioProcessor& processor;
    AudioDocument& document;
    Clipboard clipboard;

    juce::TextButton playButton   { "Play" };
    juce::ToggleButton loopButton { "Loop" };
    juce::Label timeLabel;
    juce::Label recLabel;   // "● REC m:ss" while recording, right-aligned
    juce::TextButton recordButton { "Record" };
    juce::TextButton toolsButton  { "Tools" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
