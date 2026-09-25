#include "PluginEditor.h"
#if JUCE_MAC
 #include "StandaloneWindowShape.h"
#endif

namespace
{
    // A selection dragged OUT of our own waveform and released back onto our own window must
    // NOT reload as a new document -- that's an accidental self-drop. But a selection dragged
    // from *another* R3WRK instance (a separate process, so its export isn't in flight here)
    // is a real "open this" and must go through. WaveformDisplay tracks the in-flight export
    // path per process; this just asks it.
    bool isOwnSelectionDragFile(const juce::String& path)
    {
        return WaveformDisplay::isSelfExportInFlight(path);
    }
}

R3WRKAudioProcessorEditor::R3WRKAudioProcessorEditor(R3WRKAudioProcessor& p)
    : AudioProcessorEditor(&p),
      standaloneWindow(p.wrapperType == juce::AudioProcessor::wrapperType_Standalone),
      processorRef(p),
      header(p.document), waveformDisplay(p.document, true), spectrogramDisplay(p.document),
      timeRuler(waveformDisplay, p.document), toolbar(p, p.document),
      knobRow(p.document, standaloneWindow), fxRow(p.document, standaloneWindow)
{
    juce::LookAndFeel::setDefaultLookAndFeel(&fontLnf);

    addAndMakeVisible(header);
    addAndMakeVisible(waveformDisplay);
    addChildComponent(spectrogramDisplay); // built but not currently reachable from the UI
    addAndMakeVisible(timeRuler);
    addAndMakeVisible(toolbar);
    addAndMakeVisible(knobRow);
    addChildComponent(fxRow);   // hidden until the drawer is opened -- see toggleFxDrawer()

    knobRow.onDrawerToggle = [this] { toggleFxDrawer(); };

    toolbar.onSourceNameChanged = [this](juce::String name)
    {
        header.setSourceName(name);
        if (! name.isEmpty())            // a file was opened/saved — frame the whole thing
            waveformDisplay.zoomToFit();
    };
    toolbar.onSaved = [this] { header.markSaved(); };
    toolbar.onStatusMessage = [this](juce::String m) { header.flashMessage(m); };
    header.onNameClicked = [this] { toolbar.openFile(); };

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
    // (Amplify/Fade In/Fade Out/Reverse/Stretch·Pitch), handled by the toolbar since it already owns those
    // pop-up panels and EditActions calls.
    waveformDisplay.onSelectionContextMenu = [this](juce::Point<int> screenPos)
    {
        toolbar.showSelectionContextMenu(screenPos);
    };

    // Slice tool: clicking a slice has already set the selection + playhead; kick off playback.
    waveformDisplay.onSlicePlay = [this] { processorRef.startPlayback(); };

    // Turning the Start / End knobs slides the selection -- keep it on screen when zoomed in.
    knobRow.onSelectionKnobMoved = [this] { waveformDisplay.scrollSelectionIntoView(); };

    // Follow-playhead toggle -- header row, every build (moved off the transport strip).
    followButton.setClickingTogglesState(true);
    followButton.setWantsKeyboardFocus(false);
    followButton.setTooltip("Follow playhead -- keeps the playhead on screen while playing when zoomed in");
    followButton.setLookAndFeel(&cornerButtonLnF);
    followButton.setToggleState(processorRef.document.followPlayheadEnabled, juce::dontSendNotification);
    followButton.onClick = [this]
    {
        processorRef.document.followPlayheadEnabled = followButton.getToggleState();
        processorRef.document.notifyChanged();   // WaveformDisplay's 30 Hz timer does the following
    };
    addAndMakeVisible(followButton);

    if (standaloneWindow)
    {
        floatOnTopButton.setClickingTogglesState(true);
        floatOnTopButton.setWantsKeyboardFocus(false);
        floatOnTopButton.setTooltip("Float on top - keep this window above other apps");
        floatOnTopButton.setLookAndFeel(&cornerButtonLnF);
        floatOnTopButton.onClick = [this] { applyFloatOnTop(floatOnTopButton.getToggleState()); };
        addAndMakeVisible(floatOnTopButton);
    }

    applyHeaderButtonThemes();

    theme->addChangeListener(this);

   #if JUCE_MAC
    // Standalone gets a real macOS menu bar (File / Edit / Tools) mirroring the Tools ▾
    // button. Plugins skip it -- the host owns the menu bar.
    if (standaloneWindow)
    {
        macMenuBar = std::make_unique<StandaloneMenuBar>(toolbar);
        const auto appleMenuItems = macMenuBar->getAppleMenuItems();   // JUCE copies this
        juce::MenuBarModel::setMacMainMenu(macMenuBar.get(), &appleMenuItems);
    }
   #endif

    const int topInset = standaloneWindow ? kMacTrafficLightInset : 0;
    setWantsKeyboardFocus(true);
    // false = don't draw JUCE's own bottom-right resize grip (the diagonal Windows-style
    // lines) -- the Standalone window already gets real native macOS edge/corner resizing
    // (see StandaloneWindowShape.mm: it's a real NSWindow with a native title bar under the
    // hood, just visually hidden), so the drawn grip was a redundant, non-native-looking
    // extra. Window is still resizable either way -- this only removes the drawn handle.
    setResizable(true, false);
    setResizeLimits(680, 473 + topInset, 2200, 1300 + topInset);
    setSize(1000, 639 + topInset);
}

R3WRKAudioProcessorEditor::~R3WRKAudioProcessorEditor()
{
   #if JUCE_MAC
    if (macMenuBar != nullptr)
        juce::MenuBarModel::setMacMainMenu(nullptr);   // detach before the model is destroyed
   #endif
    followButton.setLookAndFeel(nullptr);
    floatOnTopButton.setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);   // detach before fontLnf is destroyed
    theme->removeChangeListener(this);
}

void R3WRKAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyHeaderButtonThemes();
    repaint();
}

void R3WRKAudioProcessorEditor::applyHeaderButtonThemes()
{
    const auto& pal = theme->palette();
    // Boxless (see cornerButtonLnF) -- plain text ink in both states; "on" is a small dot drawn
    // under the icon by R3WRKLookAndFeel::drawButtonText.
    for (auto* b : { &followButton, &floatOnTopButton })
    {
        b->setColour(juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
        b->setColour(juce::TextButton::textColourOffId, pal.text);
        b->setColour(juce::TextButton::textColourOnId,  pal.text);
    }
}

void R3WRKAudioProcessorEditor::toggleFxDrawer()
{
    fxDrawerOpen = ! fxDrawerOpen;
    knobRow.setDrawerOpen(fxDrawerOpen);
    fxRow.setVisible(fxDrawerOpen);

    const int topInset = standaloneWindow ? kMacTrafficLightInset : 0;
    const int delta = kFxRowHeight + kFxRowGap;
    // The drawer only ever adds to the plugin's normal footprint -- min/max both shift by the
    // same delta so a host that clamps to the reported limits can't crush the open drawer, and
    // the user can't manually resize below the closed-row minimum either way.
    setResizeLimits(680, (fxDrawerOpen ? 473 + delta : 473) + topInset,
                     2200, (fxDrawerOpen ? 1300 + delta : 1300) + topInset);
    setSize(getWidth(), getHeight() + (fxDrawerOpen ? delta : -delta));
}

void R3WRKAudioProcessorEditor::applyFloatOnTop(bool on)
{
   #if JUCE_MAC
    if (standaloneWindow)
        r3wrkSetWindowFloatOnTop(this, on);
   #endif
    outputSettings->setFloatOnTop(on);
}

void R3WRKAudioProcessorEditor::maybeApplyPersistedFloatOnTop()
{
    // The native window level can only be set once a peer exists; the ctor runs too early.
    // Called from parentHierarchyChanged() and the first resized() -- whichever wins once the
    // window is actually on screen -- and guarded so it only takes effect once.
    if (! standaloneWindow || floatStateApplied || getPeer() == nullptr)
        return;

    floatStateApplied = true;
    const bool on = outputSettings->floatOnTop();
    floatOnTopButton.setToggleState(on, juce::dontSendNotification);
   #if JUCE_MAC
    r3wrkSetWindowFloatOnTop(this, on);
   #endif
}

void R3WRKAudioProcessorEditor::parentHierarchyChanged()
{
    maybeApplyPersistedFloatOnTop();
}

void R3WRKAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto& pal = theme->palette();

    if (pal.shadedPanel)
    {
        // "Panel body" look: a shadow anchored to each of the four edges, bleeding inward
        // until it reaches the waveform display -- a recessed/inset look -- in place of the
        // flat windowBg fill every other theme uses (see the shadedPanel comment on
        // Palette). Earlier attempts (a centred vignette, then a directional light-source
        // gradient) both read as flat or left-to-right: almost all of the editor's own
        // background is covered by child components, so only a thin, wide-but-short strip
        // (the header margin, mostly) ever shows windowBg directly, and any single gradient
        // spanning the whole window is dominated by that strip's width. Four edge-anchored
        // fades side-step that -- each reaches exactly as far as the gap to the waveform's
        // own edge on that side (so top/bottom, which cross the header/transport/knob rows,
        // bleed much further than the ~8px left/right margins) -- and overlap naturally at
        // the corners. Each is eased (a couple of intermediate stops) rather than a plain
        // linear ramp, for a softer edge than a straight fade gives.
        auto bounds = getLocalBounds().toFloat();
        g.fillAll(pal.windowBg);

        const auto shadow = pal.windowBg.darker(pal.edgeShadeDarken).withAlpha(pal.edgeShadeAlpha);

        auto softEdgeGradient = [&shadow](juce::Point<float> from, juce::Point<float> to)
        {
            juce::ColourGradient grad(shadow, from.x, from.y,
                                      shadow.withAlpha(0.0f), to.x, to.y, false);
            grad.addColour(0.35, shadow.withMultipliedAlpha(0.85f));
            grad.addColour(0.65, shadow.withMultipliedAlpha(0.35f));
            return grad;
        };

        const auto wave = waveformDisplay.getBounds().toFloat();
        const float minFade = 6.0f;   // guard against a degenerate (near-zero) gap before first layout
        const float topFade    = juce::jmax(minFade, wave.getY() - bounds.getY());
        const float bottomFade = juce::jmax(minFade, bounds.getBottom() - wave.getBottom());
        const float leftFade   = juce::jmax(minFade, wave.getX() - bounds.getX());
        const float rightFade  = juce::jmax(minFade, bounds.getRight() - wave.getRight());

        g.setGradientFill(softEdgeGradient({ 0, bounds.getY() }, { 0, bounds.getY() + topFade }));
        g.fillRect(bounds);

        g.setGradientFill(softEdgeGradient({ 0, bounds.getBottom() }, { 0, bounds.getBottom() - bottomFade }));
        g.fillRect(bounds);

        g.setGradientFill(softEdgeGradient({ bounds.getX(), 0 }, { bounds.getX() + leftFade, 0 }));
        g.fillRect(bounds);

        g.setGradientFill(softEdgeGradient({ bounds.getRight(), 0 }, { bounds.getRight() - rightFade, 0 }));
        g.fillRect(bounds);
    }
    else
    {
        g.fillAll(pal.windowBg);
    }

    if (showingDropHighlight)
    {
        g.setColour(theme->palette().accent);
        g.drawRect(getLocalBounds(), 3);
    }
}

void R3WRKAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
   #if JUCE_MAC
    // Standalone/macOS: the native title-bar strip is hidden, so the reserved band at the top
    // of our own UI (where the traffic lights float) stands in for a title bar -- drag it to
    // move the window, double-click it to zoom. Only arm here; the move starts on mouseDrag so
    // a double-click isn't swallowed by AppKit's nested drag loop. Child components handle
    // their own clicks and never reach here.
    windowDragArmed = standaloneWindow && e.position.y < (float) kMacTrafficLightInset;
    windowDragActive = false;
   #else
    juce::ignoreUnused(e);
   #endif
}

void R3WRKAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
   #if JUCE_MAC
    if (windowDragArmed && ! windowDragActive)
    {
        windowDragActive = true;
        r3wrkBeginWindowDrag(this);   // AppKit runs the native window-move loop from here
    }
   #else
    juce::ignoreUnused(e);
   #endif
}

void R3WRKAudioProcessorEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
   #if JUCE_MAC
    if (standaloneWindow && e.position.y < (float) kMacTrafficLightInset)
        r3wrkTitleBarDoubleClick(this);   // fill the screen (or the user's title-bar pref)
   #else
    juce::ignoreUnused(e);
   #endif
}

void R3WRKAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    if (standaloneWindow)
        area.removeFromTop(kMacTrafficLightInset);   // clear of the floating macOS traffic lights

    {
        auto headerRow = area.removeFromTop(30);
        // Mirror the file name's left inset (HeaderBar reserves 16px on its left for the dirty
        // dot before the name even starts) on this side, so the icon group's right margin
        // matches the name's left margin instead of sitting flush against the window edge.
        headerRow.removeFromRight(16);
        // Corner buttons at the far right, in line with the file name -- small rounded rects
        // (their own shape, not a pill), like RCRDR / Sieve. Float-on-top is the outermost
        // (standalone only); Follow-playhead sits just left of it.
        if (standaloneWindow)
        {
            floatOnTopButton.setBounds(headerRow.removeFromRight(36).withSizeKeepingCentre(34, 30));
            headerRow.removeFromRight(6);
        }
        followButton.setBounds(headerRow.removeFromRight(36).withSizeKeepingCentre(34, 30));
        headerRow.removeFromRight(6);
        header.setBounds(headerRow);
    }
    area.removeFromTop(6);

    // FX drawer, when open, sits under the main knob row -- removed from the bottom first so it
    // lands below knobRow rather than displacing it. The window itself grew by this same
    // (height + gap) when the drawer opened (see toggleFxDrawer()), so nothing above has to
    // shrink to make room for it.
    if (fxDrawerOpen)
    {
        fxRow.setBounds(area.removeFromBottom(kFxRowHeight));
        area.removeFromBottom(kFxRowGap);
    }

    knobRow.setBounds(area.removeFromBottom(83));   // knob strip, under the transport bar --
        // 74 + 9 for the bigger caption/readout fonts (experiment/space-mono-font), so the
        // rotary disc gets that space back instead of losing it to the taller text
    area.removeFromBottom(4);
    toolbar.setBounds(area.removeFromBottom(38));   // transport control strip (dark band + padding)
    area.removeFromBottom(6);

    timeRuler.setBounds(area.removeFromBottom(22)); // time ruler, hugging the waveform
    area.removeFromBottom(2);

    waveformDisplay.setBounds(area);
    spectrogramDisplay.setBounds(area);

    maybeApplyPersistedFloatOnTop();
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
    if (key == KP('s', cmd, 0))               { toolbar.saveInPlace(); return true; }

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
    // A selection dragged out of our own waveform, released back over our window -> ignore it
    // (the user was aiming at another app and missed). Dragging out still works.
    for (auto& f : files)
        if (isOwnSelectionDragFile(f))
            return false;

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
        if (isOwnSelectionDragFile(f))
            return;   // our own selection export, dropped back onto us -- ignore (see above)

        juce::File file(f);
        if (file.hasFileExtension("wav;aiff;aif;flac;ogg;mp3"))
        {
            toolbar.loadAudioFile(file);
            return;   // one file at a time, same as Open -- the first recognised one dropped
        }
    }
}
