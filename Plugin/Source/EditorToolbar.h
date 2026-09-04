#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "EditActions.h"
#include "PluginProcessor.h"

/**
    The control surface below the waveform, in the same shape as Sieve's audio editor:
      row 1  transport   — Play / Loop / time  ....  record meter + Record
      row 2  operations  — Trim Delete Silence Normalize Amplify… Reverse Fade In/Out │ Cut Copy Paste │ ↶ ↷
      row 3  file        — Open… / Save As… / Revert  ·  Tools ▾ (Stretch/Pitch, Chop-to-Grid, Export Slices)

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

    std::function<void()> onZoomIn, onZoomOut;
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

    // row 1 — transport
    juce::TextButton recordButton { "Record" };
    juce::TextButton playButton   { "Play" };
    juce::ToggleButton loopButton { "Loop" };
    juce::Label timeLabel;
    juce::Label recLabel;   // "● 0:05" while recording, right-aligned

    // row 2 — operations
    juce::TextButton trimButton      { "Trim" };
    juce::TextButton deleteButton    { "Delete" };
    juce::TextButton silenceButton   { "Silence" };
    juce::TextButton normalizeButton { "Normalize" };
    juce::TextButton amplifyButton   { "Amplify..." };
    juce::TextButton reverseButton   { "Reverse" };
    juce::TextButton fadeInButton    { "Fade In" };
    juce::TextButton fadeOutButton   { "Fade Out" };
    juce::TextButton cutButton       { "Cut" };
    juce::TextButton copyButton      { "Copy" };
    juce::TextButton pasteButton     { "Paste" };
    juce::TextButton undoButton      { "Undo" };
    juce::TextButton redoButton      { "Redo" };

    // row 3 — file
    juce::TextButton openButton   { "Open..." };
    juce::TextButton saveButton   { "Save As..." };
    juce::TextButton revertButton { "Revert" };
    juce::TextButton toolsButton  { "Tools" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
