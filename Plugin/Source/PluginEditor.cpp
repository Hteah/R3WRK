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

    setResizable(true, true);
    setResizeLimits(760, 420, 2200, 1300);
    setSize(1180, 640);
}

EdisonCloneAudioProcessorEditor::~EdisonCloneAudioProcessorEditor() = default;

void EdisonCloneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff14161a));
}

void EdisonCloneAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    toolbar.setBounds(area.removeFromTop(96));
    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);
}
