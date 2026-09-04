#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "EditActions.h"
#include "PluginProcessor.h"

/**
    Every button/slider the editor exposes: transport, file, edit, processing,
    time-stretch/pitch, chop-to-grid and loop tools. Owns the clipboard and talks
    to the AudioDocument (and the processor, for record/play transport) directly;
    exposes a couple of callbacks for things it doesn't own (view toggle, zoom).
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

private:
    void timerCallback() override;
    void updateTransportButtonText();
    void openFile();
    void saveFile();
    void exportSlices();
    void applyTimeStretch();

    EdisonCloneAudioProcessor& processor;
    AudioDocument& document;
    Clipboard clipboard;

    // Row 1: transport + file + undo
    juce::TextButton recordButton { "Record" };
    juce::TextButton playButton   { "Play" };
    juce::ToggleButton loopButton { "Loop" };
    juce::TextButton openButton   { "Open..." };
    juce::TextButton saveButton   { "Save As..." };
    juce::TextButton undoButton   { "Undo" };
    juce::TextButton redoButton   { "Redo" };
    juce::TextButton waveViewButton { "Waveform" };
    juce::TextButton specViewButton { "Spectrogram" };
    juce::TextButton zoomInButton  { "Zoom +" };
    juce::TextButton zoomOutButton { "Zoom -" };
    juce::TextButton zoomFitButton { "Zoom Fit" };

    // Row 2: edit + processing
    juce::TextButton cutButton    { "Cut" };
    juce::TextButton copyButton   { "Copy" };
    juce::TextButton pasteButton  { "Paste" };
    juce::TextButton trimButton   { "Trim" };
    juce::TextButton deleteButton { "Delete" };
    juce::TextButton normalizeButton { "Normalize" };
    juce::TextButton fadeInButton  { "Fade In" };
    juce::TextButton fadeOutButton { "Fade Out" };
    juce::TextButton reverseButton { "Reverse" };
    juce::TextButton silenceButton { "Silence" };
    juce::Slider gainSlider;
    juce::TextButton applyGainButton { "Apply Gain" };

    // Row 3: time-stretch / pitch, chop-to-grid, loop
    juce::Label stretchLabel { {}, "Stretch x" };
    juce::Slider stretchSlider;
    juce::Label pitchLabel { {}, "Pitch st" };
    juce::Slider pitchSlider;
    juce::TextButton applyStretchButton { "Apply Stretch/Pitch" };

    juce::Label bpmLabel { {}, "BPM" };
    juce::Slider bpmSlider;
    juce::ComboBox divisionBox;
    juce::TextButton recalcGridButton { "Update Grid" };
    juce::TextButton exportSlicesButton { "Export Slices..." };

    juce::TextButton setLoopStartButton { "Loop = Sel Start" };
    juce::TextButton setLoopEndButton   { "Loop = Sel End" };

    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorToolbar)
};
