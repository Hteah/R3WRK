#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "EditActions.h"
#include "PluginProcessor.h"

/**
    The compact toolbar: transport + file + undo + a single view toggle + zoom-fit,
    with everything else (clipboard, region processing, stretch/pitch, chop-to-grid,
    loop) tucked behind the "Edit" menu button. Owns the clipboard and talks to the
    AudioDocument / processor directly.
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

    std::function<void()> onZoomIn, onZoomOut, onZoomFit;
    std::function<void(bool showSpectrogram)> onViewModeChanged;

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
    void showEditMenu();
    void showGainCallout();
    void showStretchCallout();
    void showChopCallout();
    void exportSlices();

    EdisonCloneAudioProcessor& processor;
    AudioDocument& document;
    Clipboard clipboard;
    bool showingSpectrogram = false;

    juce::TextButton recordButton  { "Record" };
    juce::TextButton playButton    { "Play" };
    juce::ToggleButton loopButton  { "Loop" };
    juce::TextButton openButton    { "Open\xE2\x80\xA6" };
    juce::TextButton saveButton    { "Save\xE2\x80\xA6" };
    juce::TextButton undoButton    { "Undo" };
    juce::TextButton redoButton    { "Redo" };
    juce::TextButton viewButton    { "Spectrogram" };
    juce::TextButton zoomFitButton { "Fit" };
    juce::TextButton editMenuButton { "Edit \xE2\x96\xBE" };
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
