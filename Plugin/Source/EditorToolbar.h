#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "EditActions.h"
#include "PluginProcessor.h"
#include "Theme.h"
#include "OutputSettings.h"
#include "R3WRKLookAndFeel.h"

/**
    A single control strip below the waveform:

        Play · Loop · time    ···    ● REC m:ss · Record · Tools ▾

    Everything else — file ops, clipboard, region processing, stretch/pitch,
    export selection, undo/redo — lives in the "Tools" pop-up menu.
    Owns the clipboard and talks to the AudioDocument / processor directly.
*/
class EditorToolbar : public juce::Component,
                      public juce::ChangeListener,
                      private juce::Timer
{
public:
    EditorToolbar(R3WRKAudioProcessor& processor, AudioDocument& document);
    ~EditorToolbar() override;

    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    std::function<void(juce::String name)> onSourceNameChanged;   // "" for a fresh recording
    std::function<void()> onSaved;                                // after a successful open / save
    std::function<void(juce::String message)> onStatusMessage;    // brief header status line

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
    void applyTheme();
    void updateTransportButtonText();
    void openFile();
    void saveFile();
    void revertAll();
    void showToolsMenu();
    void showAmplifyCallout();
    void showStretchCallout();
    void showThemeCallout();
    void chooseOutputFolder();
    void exportSelectionToFolder();
    void autoSaveRecording();

    R3WRKAudioProcessor& processor;
    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    juce::SharedResourcePointer<OutputSettings> outputSettings;
    R3WRKLookAndFeel toolbarLnF;
    Clipboard clipboard;

    juce::TextButton playButton   { "Play" };
    juce::TextButton loopButton   { "Loop" };   // setClickingTogglesState(true) -- a toggling
                                                // pill, not a checkbox, to match Play/Record/Tools
    juce::Label timeLabel;
    juce::Label recLabel;   // "● REC m:ss" while recording, right-aligned
    juce::TextButton recordButton { "Record" };
    juce::TextButton toolsButton  { "Tools" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
