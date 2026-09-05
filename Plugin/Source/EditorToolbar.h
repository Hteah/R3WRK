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

        ⏮ Play · Loop · Scrub · time    ···    ● REC m:ss · Record · Tools

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

    void paint(juce::Graphics&) override;
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
    void playFromStart();     // restarts playback at sample 0 (region-snapped, see processBlock)

    // Right-click inside a selection (WaveformDisplay::onSelectionContextMenu) -> a small
    // menu of the region-processing ops that make sense on a selection: Amplify, Reverse,
    // Stretch/Pitch. Reverse runs immediately (same as Tools ▾); the other two open the same
    // pop-up panels Tools ▾ uses, anchored at the click instead of the Tools button.
    void showSelectionContextMenu(juce::Point<int> screenPosition);

private:
    void timerCallback() override;
    void applyTheme();
    void updateTransportButtonText();
    void openFile();
    void saveFile();
    void revertAll();
    void showToolsMenu();
    void showAmplifyCallout(juce::Rectangle<int> screenTargetArea);
    void showStretchCallout(juce::Rectangle<int> screenTargetArea);
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

    // Play/Loop/Record are drawn as icons (see R3WRKLookAndFeel::drawButtonText) on square
    // (so pill-radius-as-circle) buttons -- a play triangle, a loop/repeat glyph, and (for
    // Record) no icon at all while idle, since a plain red circular button already reads as
    // "record" on its own; a stop square appears on it once recording.
    juce::TextButton playFromStartButton { R3WRKLookAndFeel::iconPlayFromStart };
    juce::TextButton playButton   { R3WRKLookAndFeel::iconPlay };
    juce::TextButton loopButton   { R3WRKLookAndFeel::iconLoop };   // setClickingTogglesState(true)
                                                                    // -- a toggling pill, not a checkbox
    juce::TextButton scrubButton  { R3WRKLookAndFeel::iconScrub };  // setClickingTogglesState(true)
                                                                    // -- toggles the scrub *tool*;
                                                                    // WaveformDisplay does the actual
                                                                    // dragging once it's on
    juce::Label timeLabel;   // also carries the "● REC m:ss" elapsed time while recording
    juce::TextButton recordButton;
    juce::TextButton toolsButton  { R3WRKLookAndFeel::iconTools };   // opens the Tools ▾ pop-up menu

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
