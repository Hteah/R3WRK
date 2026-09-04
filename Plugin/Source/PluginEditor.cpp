#include "PluginEditor.h"

R3WRKAudioProcessorEditor::R3WRKAudioProcessorEditor(R3WRKAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p),
      header(p.document), waveformDisplay(p.document), spectrogramDisplay(p.document),
      toolbar(p, p.document), knobRow(p, p.document)
{
    addAndMakeVisible(header);
    addAndMakeVisible(waveformDisplay);
    addChildComponent(spectrogramDisplay); // hidden until the user switches views
    addAndMakeVisible(toolbar);
    addAndMakeVisible(knobRow);

    header.onZoomFit = [this] { waveformDisplay.zoomToFit(); };
    header.onViewModeChanged = [this](bool spectrogram)
    {
        showingSpectrogram = spectrogram;
        waveformDisplay.setVisible(! spectrogram);
        spectrogramDisplay.setVisible(spectrogram);
    };

    toolbar.onSourceNameChanged = [this](juce::String name)
    {
        header.setSourceName(name);
        if (! name.isEmpty())            // a file was opened/saved — frame the whole thing
            waveformDisplay.zoomToFit();
    };
    toolbar.onSaved = [this] { header.markSaved(); };

    // A committed drag-selection / edge-resize drops the playhead at the selection start,
    // so Play picks up from there (Sieve's editor jumps playback on selection commit).
    waveformDisplay.onSelectionCommitted = [this]
    {
        auto& d = processorRef.document;
        if (d.hasSelection())
        {
            d.playhead = d.getSelectionStart();
            d.notifyChanged();
        }
    };

    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(680, 430, 2200, 1300);
    setSize(1000, 620);
}

R3WRKAudioProcessorEditor::~R3WRKAudioProcessorEditor() = default;

void R3WRKAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff14161a));
}

void R3WRKAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    header.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);

    knobRow.setBounds(area.removeFromBottom(74));   // knob strip, under the transport bar
    area.removeFromBottom(4);
    toolbar.setBounds(area.removeFromBottom(28));   // transport control strip
    area.removeFromBottom(6);

    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);
}

bool R3WRKAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
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
