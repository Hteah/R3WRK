#include "PluginEditor.h"

R3WRKAudioProcessorEditor::R3WRKAudioProcessorEditor(R3WRKAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p),
      header(p.document), waveformDisplay(p.document), spectrogramDisplay(p.document),
      timeRuler(waveformDisplay, p.document), toolbar(p, p.document), knobRow(p.document)
{
    addAndMakeVisible(header);
    addAndMakeVisible(waveformDisplay);
    addChildComponent(spectrogramDisplay); // built but not currently reachable from the UI
    addAndMakeVisible(timeRuler);
    addAndMakeVisible(toolbar);
    addAndMakeVisible(knobRow);

    toolbar.onSourceNameChanged = [this](juce::String name)
    {
        header.setSourceName(name);
        if (! name.isEmpty())            // a file was opened/saved — frame the whole thing
            waveformDisplay.zoomToFit();
    };
    toolbar.onSaved = [this] { header.markSaved(); };
    toolbar.onStatusMessage = [this](juce::String m) { header.flashMessage(m); };

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

    // Right-click inside the selection -> a small menu of region-processing ops
    // (Amplify/Reverse/Stretch·Pitch), handled by the toolbar since it already owns those
    // pop-up panels and EditActions calls.
    waveformDisplay.onSelectionContextMenu = [this](juce::Point<int> screenPos)
    {
        toolbar.showSelectionContextMenu(screenPos);
    };

    theme->addChangeListener(this);

    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(680, 464, 2200, 1300);
    setSize(1000, 630);
}

R3WRKAudioProcessorEditor::~R3WRKAudioProcessorEditor()
{
    theme->removeChangeListener(this);
}

void R3WRKAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme->palette().windowBg);
}

void R3WRKAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    header.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);

    knobRow.setBounds(area.removeFromBottom(74));   // knob strip, under the transport bar
    area.removeFromBottom(4);
    toolbar.setBounds(area.removeFromBottom(38));   // transport control strip (dark band + padding)
    area.removeFromBottom(6);

    timeRuler.setBounds(area.removeFromBottom(22)); // time ruler, hugging the waveform
    area.removeFromBottom(2);

    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);
}

bool R3WRKAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto cmdShift = cmd | juce::ModifierKeys::shiftModifier;
    const auto ctrl = juce::ModifierKeys::ctrlModifier;   // literally Control, not ⌘ -- distinct
                                                          // bits on macOS (unlike Windows/Linux,
                                                          // where JUCE aliases them)

    if (key == KP(juce::KeyPress::spaceKey))  { toolbar.togglePlay(); return true; }
    if (key == KP('z', cmd, 0))               { toolbar.doUndo();     return true; }
    if (key == KP('z', cmdShift, 0))          { toolbar.doRedo();     return true; }
    if (key == KP('x', cmd, 0))               { toolbar.doCut();      return true; }
    if (key == KP('c', cmd, 0))               { toolbar.doCopy();     return true; }
    if (key == KP('v', cmd, 0))               { toolbar.doPaste();    return true; }

    // Zoom the waveform without the mouse. Ctrl+Up/Down are the reliable primary binding: on
    // macOS, Control held with a *printable symbol* key ("+"/"-"/"=") never reaches this
    // function at all on a US keyboard -- Control has no defined control-code mapping for
    // those characters, so Cocoa's own text layer decides there's nothing to insert,
    // [NSEvent characters] comes back empty, and JUCE's Cocoa backend only calls
    // handleKeyPress() by iterating that (now-empty) string, so the native event is dropped
    // before any JUCE KeyPress is ever constructed -- not something fixable by how we match
    // it here. Arrow keys don't hit that path (Cocoa always reports a non-empty character for
    // them), so Ctrl+Up/Down get through reliably. Ctrl+"+"/"-" are still matched below too,
    // in case a different keyboard layout doesn't hit the same snag.
    if (key == KP(KP::upKey, ctrl, 0))   { waveformDisplay.zoomIn();  return true; }
    if (key == KP(KP::downKey, ctrl, 0)) { waveformDisplay.zoomOut(); return true; }
    if (key.getModifiers().isCtrlDown())
    {
        const auto ch = key.getTextCharacter();
        if (ch == '+' || ch == '=') { waveformDisplay.zoomIn();  return true; }
        if (ch == '-')              { waveformDisplay.zoomOut(); return true; }
    }
    return false;
}
