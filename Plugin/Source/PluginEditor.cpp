#include "PluginEditor.h"

EdisonCloneAudioProcessorEditor::EdisonCloneAudioProcessorEditor(EdisonCloneAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p),
      header(p.document), waveformDisplay(p.document), spectrogramDisplay(p.document),
      toolbar(p, p.document)
{
    addAndMakeVisible(header);
    addAndMakeVisible(waveformDisplay);
    addChildComponent(spectrogramDisplay); // hidden until the user switches views
    addAndMakeVisible(toolbar);

    header.onZoomFit = [this] { waveformDisplay.zoomToFit(); };
    header.onViewModeChanged = [this](bool spectrogram)
    {
        showingSpectrogram = spectrogram;
        waveformDisplay.setVisible(! spectrogram);
        spectrogramDisplay.setVisible(spectrogram);
    };

    toolbar.onSourceNameChanged = [this](juce::String name) { header.setSourceName(name); };
    toolbar.onSaved             = [this] { header.markSaved(); };

    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(680, 340, 2200, 1300);
    setSize(1000, 560);
}

EdisonCloneAudioProcessorEditor::~EdisonCloneAudioProcessorEditor() = default;

void EdisonCloneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff14161a));
}

void EdisonCloneAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    header.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);

    // three control rows (26 px each) + gaps, pinned to the bottom
    toolbar.setBounds(area.removeFromBottom(3 * 26 + 2 * 4));
    area.removeFromBottom(6);

    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);
}

bool EdisonCloneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto cmdShift = cmd | juce::ModifierKeys::shiftModifier;

    if (key == KP(juce::KeyPress::spaceKey))  { toolbar.togglePlay(); return true; }
    if (key == KP('z', cmd, 0))               { toolbar.doUndo();     return true; }
    if (key == KP('z', cmdShift, 0))          { toolbar.doRedo();     return true; }
    if (key == KP('x', cmd, 0))               { toolbar.doCut();      return true; }
    if (key == KP('c', cmd, 0))               { toolbar.doCopy();     return true; }
    if (key == KP('v', cmd, 0))               { toolbar.doPaste();    return true; }
    return false;
}
