#include "PluginEditor.h"

R3WRKAudioProcessorEditor::R3WRKAudioProcessorEditor(R3WRKAudioProcessor& p)
    : AudioProcessorEditor(&p),
      standaloneWindow(p.wrapperType == juce::AudioProcessor::wrapperType_Standalone),
      processorRef(p),
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

    // Slice tool: clicking a slice has already set the selection + playhead; kick off playback.
    waveformDisplay.onSlicePlay = [this] { processorRef.startPlayback(); };

    theme->addChangeListener(this);

    const int topInset = standaloneWindow ? kMacTrafficLightInset : 0;
    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(680, 464 + topInset, 2200, 1300 + topInset);
    setSize(1000, 630 + topInset);
}

R3WRKAudioProcessorEditor::~R3WRKAudioProcessorEditor()
{
    theme->removeChangeListener(this);
}

void R3WRKAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme->palette().windowBg);

    if (showingDropHighlight)
    {
        g.setColour(theme->palette().accent);
        g.drawRect(getLocalBounds(), 3);
    }
}

void R3WRKAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    if (standaloneWindow)
        area.removeFromTop(kMacTrafficLightInset);   // clear of the floating macOS traffic lights

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

    if (key == KP(juce::KeyPress::spaceKey))  { toolbar.togglePlay(); return true; }
    if (key == KP('z', cmd, 0))               { toolbar.doUndo();     return true; }
    if (key == KP('z', cmdShift, 0))          { toolbar.doRedo();     return true; }
    if (key == KP('x', cmd, 0))               { toolbar.doCut();      return true; }
    if (key == KP('c', cmd, 0))               { toolbar.doCopy();     return true; }
    if (key == KP('v', cmd, 0))               { toolbar.doPaste();    return true; }
    if (key == KP('t', cmd, 0))               { toolbar.doTrim();     return true; }

    // Zoom the waveform without the mouse: the standard Mac convention, ⌘+/⌘- (Safari,
    // Preview, Xcode, ...). Two earlier attempts used Control instead (per the user's first
    // request) and both turned out to be dead ends on macOS: Control held with a symbol key
    // ("+"/"-"/"=") never reaches a JUCE KeyPress at all (Control has no control-code mapping
    // for those characters, so Cocoa's text layer produces no characters and JUCE's Cocoa
    // backend drops the event before dispatch), and Control+Up/Down are themselves claimed
    // system-wide for Mission Control / Application Windows, intercepted before any app sees
    // them. ⌘ avoids both problems -- matched on keyCode, not the resulting character/text,
    // same as the ⌘Z/⌘X/⌘C/⌘V shortcuts above: JUCE's Cocoa backend zeroes the text character
    // for every ⌘ chord (it's a command, not text input), so keyCode is the only thing to
    // match on. ⌘+ arrives as either Shift-⌘-"=" (cmdShift, the usual case on a US keyboard)
    // or occasionally its own "+" keycode, so both are matched alongside plain ⌘-"=".
    if (key == KP('=', cmd, 0) || key == KP('=', cmdShift, 0) || key == KP('+', cmd, 0))
        { waveformDisplay.zoomIn(); return true; }
    if (key == KP('-', cmd, 0))
        { waveformDisplay.zoomOut(); return true; }

    // Left/Right arrows scroll through the waveform (the playhead -- the red line -- rides along
    // while stopped). Shift = half a screen per press instead of ~1/20. Auto-repeat on hold.
    const auto shift = juce::ModifierKeys::shiftModifier;
    if (key == KP(juce::KeyPress::leftKey))            { waveformDisplay.keyboardScroll(-1, false); return true; }
    if (key == KP(juce::KeyPress::rightKey))           { waveformDisplay.keyboardScroll(+1, false); return true; }
    if (key == KP(juce::KeyPress::leftKey,  shift, 0)) { waveformDisplay.keyboardScroll(-1, true);  return true; }
    if (key == KP(juce::KeyPress::rightKey, shift, 0)) { waveformDisplay.keyboardScroll(+1, true);  return true; }
    return false;
}

bool R3WRKAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    // Same extensions Tools ▾ -> "Open…"'s FileChooser filters to (EditorToolbar::openFile()).
    for (auto& f : files)
        if (juce::File(f).hasFileExtension("wav;aiff;aif;flac;ogg;mp3"))
            return true;
    return false;
}

void R3WRKAudioProcessorEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    showingDropHighlight = true;
    repaint();
}

void R3WRKAudioProcessorEditor::fileDragExit(const juce::StringArray&)
{
    showingDropHighlight = false;
    repaint();
}

void R3WRKAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    showingDropHighlight = false;
    repaint();

    for (auto& f : files)
    {
        juce::File file(f);
        if (file.hasFileExtension("wav;aiff;aif;flac;ogg;mp3"))
        {
            toolbar.loadAudioFile(file);
            return;   // one file at a time, same as Open -- the first recognised one dropped
        }
    }
}
