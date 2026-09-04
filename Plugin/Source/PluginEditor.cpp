#include "PluginEditor.h"

EdisonCloneAudioProcessorEditor::EdisonCloneAudioProcessorEditor(EdisonCloneAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p),
      toolbar(p, p.document), waveformDisplay(p.document), spectrogramDisplay(p.document)
{
    addAndMakeVisible(toolbar);
    addAndMakeVisible(waveformDisplay);
    addChildComponent(spectrogramDisplay); // hidden until the user switches views

    toolbar.onZoomIn  = [this] { waveformDisplay.zoomIn(); };
    toolbar.onZoomOut = [this] { waveformDisplay.zoomOut(); };
    toolbar.onZoomFit = [this] { waveformDisplay.zoomToFit(); };
    toolbar.onViewModeChanged = [this](bool spectrogram)
    {
        showingSpectrogram = spectrogram;
        waveformDisplay.setVisible(! spectrogram);
        spectrogramDisplay.setVisible(spectrogram);
    };

    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(620, 320, 2200, 1300);
    setSize(1000, 560);
}

EdisonCloneAudioProcessorEditor::~EdisonCloneAudioProcessorEditor() = default;

void EdisonCloneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff14161a));
}

void EdisonCloneAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    toolbar.setBounds(area.removeFromTop(36));
    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);
}

bool EdisonCloneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto cmdShift = cmd | juce::ModifierKeys::shiftModifier;

    if (key == KP(juce::KeyPress::spaceKey))          { toolbar.togglePlay(); return true; }
    if (key == KP('z', cmd, 0))                       { toolbar.doUndo();     return true; }
    if (key == KP('z', cmdShift, 0))                  { toolbar.doRedo();     return true; }
    if (key == KP('x', cmd, 0))                       { toolbar.doCut();      return true; }
    if (key == KP('c', cmd, 0))                       { toolbar.doCopy();     return true; }
    if (key == KP('v', cmd, 0))                       { toolbar.doPaste();    return true; }
    return false;
}
