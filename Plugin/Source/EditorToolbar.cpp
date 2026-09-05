#include "EditorToolbar.h"
#include "TimeStretchEngine.h"
#include "ThemeEditor.h"

namespace
{
    juce::String formatTime(double seconds)
    {
        int mins = (int) (seconds / 60.0);
        double secs = seconds - mins * 60.0;
        return juce::String::formatted("%02d:%06.3f", mins, secs);
    }

    juce::String mmss(double seconds)
    {
        int s = (int) std::round(seconds);
        return juce::String::formatted("%d:%02d", s / 60, s % 60);
    }

    void dismissEnclosingCallout(juce::Component& c)
    {
        if (auto* box = c.findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    //==============================================================================
    struct AmplifyPanel : juce::Component
    {
        explicit AmplifyPanel(AudioDocument& doc) : document(doc)
        {
            title.setText("Amplify", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            gain.setRange(-48.0, 24.0, 0.1);
            gain.setValue(0.0);
            gain.setTextValueSuffix(" dB");
            gain.setSliderStyle(juce::Slider::LinearHorizontal);
            gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
            // Live preview: WaveformDisplay redraws the selection at this gain as the slider
            // moves (see AudioDocument::previewActive), reverted -- see the destructor below --
            // whether the user clicks Apply or dismisses the panel without applying.
            gain.onValueChange = [this]
            {
                document.previewActive = true;
                document.previewGainLinear = juce::Decibels::decibelsToGain((float) gain.getValue());
                document.previewStretchRatio = 1.0;
                document.notifyChanged();
            };
            apply.onClick = [this]
            {
                EditActions::applyGainDb(document, (float) gain.getValue());
                dismissEnclosingCallout(*this);
            };
            addAndMakeVisible(title);
            addAndMakeVisible(gain);
            addAndMakeVisible(apply);
            setSize(280, 86);
        }
        ~AmplifyPanel() override
        {
            document.previewActive = false;
            document.previewGainLinear = 1.0f;
            document.notifyChanged();
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(18));
            r.removeFromTop(6);
            apply.setBounds(r.removeFromRight(64).reduced(0, 2));
            r.removeFromRight(6);
            gain.setBounds(r);
        }
        AudioDocument& document;
        juce::Label title;
        juce::Slider gain;
        juce::TextButton apply { "Apply" };
    };

    //==============================================================================
    struct StretchPanel : juce::Component
    {
        explicit StretchPanel(AudioDocument& doc) : document(doc)
        {
            title.setText("Stretch / Pitch", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));

            lengthLabel.setText("Length", juce::dontSendNotification);
            stretch.setRange(0.25, 4.0, 0.01);
            stretch.setValue(1.0);
            stretch.setSkewFactorFromMidPoint(1.0);
            stretch.setTextValueSuffix(" x");
            stretch.setSliderStyle(juce::Slider::LinearHorizontal);
            stretch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
            // Live preview: WaveformDisplay redraws the selection stretched to this ratio as
            // the slider moves (see AudioDocument::previewActive) -- pitch has no separate
            // visual (it doesn't change the waveform's duration/shape in our preview model).
            stretch.onValueChange = [this]
            {
                document.previewActive = true;
                document.previewStretchRatio = stretch.getValue();
                document.previewGainLinear = 1.0f;
                document.notifyChanged();
            };

            pitchLabel.setText("Pitch", juce::dontSendNotification);
            pitch.setRange(-24.0, 24.0, 0.1);
            pitch.setValue(0.0);
            pitch.setTextValueSuffix(" st");
            pitch.setSliderStyle(juce::Slider::LinearHorizontal);
            pitch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);

            hint.setText("Applies to the selection, or the whole clip if nothing is selected.",
                         juce::dontSendNotification);
            hint.setFont(juce::FontOptions(11.0f));
            hint.setColour(juce::Label::textColourId, juce::Colours::grey);

            apply.onClick = [this] { run(); dismissEnclosingCallout(*this); };

            addAndMakeVisible(title);
            addAndMakeVisible(lengthLabel);
            addAndMakeVisible(stretch);
            addAndMakeVisible(pitchLabel);
            addAndMakeVisible(pitch);
            addAndMakeVisible(hint);
            addAndMakeVisible(apply);
            setSize(330, 150);
        }
        ~StretchPanel() override
        {
            document.previewActive = false;
            document.previewStretchRatio = 1.0;
            document.notifyChanged();
        }

        void run()
        {
            auto range = document.getEffectiveRange();
            if (range.getLength() <= 0)
                return;
            auto& buf = document.getBuffer();
            juce::AudioBuffer<float> region(buf.getNumChannels(), (int) range.getLength());
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                region.copyFrom(ch, 0, buf, ch, (int) range.getStart(), (int) range.getLength());
            auto stretched = TimeStretchEngine::process(region, document.getSampleRate(),
                                                        stretch.getValue(), pitch.getValue());
            if (stretched.getNumSamples() > 0)
                EditActions::replaceRangeWith(document, range, stretched, "Time Stretch/Pitch");
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(18));
            r.removeFromTop(6);
            auto row = r.removeFromTop(24);
            lengthLabel.setBounds(row.removeFromLeft(48));
            stretch.setBounds(row);
            r.removeFromTop(6);
            row = r.removeFromTop(24);
            pitchLabel.setBounds(row.removeFromLeft(48));
            pitch.setBounds(row);
            r.removeFromTop(8);
            apply.setBounds(r.removeFromTop(24).removeFromRight(72));
            r.removeFromTop(4);
            hint.setBounds(r);
        }

        AudioDocument& document;
        juce::Label title, lengthLabel, pitchLabel, hint;
        juce::Slider stretch, pitch;
        juce::TextButton apply { "Apply" };
    };

    //==============================================================================
    // No Apply button, unlike Amplify/Stretch above -- this sets a monitoring threshold, not
    // an audio edit, so it just writes straight through as the slider moves and there's
    // nothing to preview or commit.
    struct AutoRecordThresholdPanel : juce::Component
    {
        explicit AutoRecordThresholdPanel(AudioDocument& doc) : document(doc)
        {
            title.setText("Auto-Record Threshold", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));

            threshold.setRange(-60.0, 0.0, 0.5);
            threshold.setValue(document.autoRecordThresholdDb.load(), juce::dontSendNotification);
            threshold.setTextValueSuffix(" dB");
            threshold.setSliderStyle(juce::Slider::LinearHorizontal);
            threshold.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
            threshold.onValueChange = [this] { document.autoRecordThresholdDb = threshold.getValue(); };

            hint.setText("Auto-Record starts for real once the input peaks past this level.",
                         juce::dontSendNotification);
            hint.setFont(juce::FontOptions(11.0f));
            hint.setColour(juce::Label::textColourId, juce::Colours::grey);

            addAndMakeVisible(title);
            addAndMakeVisible(threshold);
            addAndMakeVisible(hint);
            setSize(300, 78);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(18));
            r.removeFromTop(6);
            threshold.setBounds(r.removeFromTop(24));
            r.removeFromTop(4);
            hint.setBounds(r);
        }
        AudioDocument& document;
        juce::Label title, hint;
        juce::Slider threshold;
    };
}

//==============================================================================
EditorToolbar::EditorToolbar(R3WRKAudioProcessor& proc, AudioDocument& doc)
    : processor(proc), document(doc)
{
    addAndMakeVisible(playFromStartButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(scrubButton);
    addAndMakeVisible(timeLabel);
    addAndMakeVisible(recordButton);
    addAndMakeVisible(toolsButton);
    addAndMakeVisible(reverseButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(autoRecordButton);

    timeLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    timeLabel.setJustificationType(juce::Justification::centredRight);
    loopButton.setClickingTogglesState(true);
    scrubButton.setClickingTogglesState(true);
    autoRecordButton.setClickingTogglesState(true);

    playFromStartButton.setTooltip("Play from start");
    playButton.setTooltip("Play the selection (Space)");
    loopButton.setTooltip("Loop");
    scrubButton.setTooltip("Scrub -- press and drag left/right; how far you pull sets the "
                           "speed, like a tape deck's shuttle wheel");
    recordButton.setTooltip("Record");
    toolsButton.setTooltip("Tools");
    reverseButton.setTooltip("Reverse the selection (or the whole clip, if nothing's selected)");
    clearButton.setTooltip("Clear -- empties the waveform and resets Pitch/Speed/Stretch/Start/End");
    autoRecordButton.setTooltip("Auto-Record -- arm it and recording starts on its own once the "
                                "input crosses the threshold (set in Tools, Auto-Record Threshold)");

    for (auto* b : { &playFromStartButton, &playButton, &loopButton, &scrubButton, &recordButton,
                     &toolsButton, &reverseButton, &clearButton, &autoRecordButton })
    {
        b->setLookAndFeel(&toolbarLnF);

        // A mouse click leaves keyboard focus sitting on whichever button was clicked, and on
        // macOS (with Full Keyboard Access on) a focused button's own accessibility "press"
        // action fires again on Space -- so e.g. clicking Loop, then hitting Space to
        // start/stop playback, would re-toggle Loop instead of reaching PluginEditor's Space
        // handler. These are a transport strip, not a tab-navigable form, so keep keyboard
        // focus off them entirely and let Space always mean "toggle play".
        b->setWantsKeyboardFocus(false);
    }

    recordButton.onClick        = [this] { toggleTransport(); };
    playButton.onClick          = [this] { togglePlay(); };
    playFromStartButton.onClick = [this] { playFromStart(); };
    loopButton.onClick          = [this] { document.loopEnabled = loopButton.getToggleState(); };
    toolsButton.onClick         = [this] { showToolsMenu(); };
    scrubButton.onClick         = [this]
    {
        document.scrubModeEnabled = scrubButton.getToggleState();
        if (document.scrubModeEnabled)
        {
            // Scrubbing and normal playback are mutually exclusive -- entering the tool
            // stops any ongoing playback, same as starting a recording already does.
            if (document.isPlaying.load())
                processor.stopPlayback();
        }
        else
        {
            // Leaving the tool mid-drag (e.g. the button toggled off from elsewhere) should
            // silence immediately, not leave the audio thread reading at a stale velocity.
            document.isScrubbing = false;
            document.scrubVelocity = 0.0;
        }
        document.notifyChanged();
    };
    reverseButton.onClick = [this] { EditActions::reverse(document); };
    clearButton.onClick = [this]
    {
        if (document.isPlaying.load())
            processor.stopPlayback();

        // Keep the current channel count/sample rate (this isn't "close the file", just
        // "start over with a blank canvas") -- newEmptyDocument() already resets
        // Pitch/Speed/Stretch and the selection/loop/playhead to identity, the same reset a
        // freshly loaded or recorded file gets.
        const int numCh = document.getBuffer().getNumChannels() > 0 ? document.getBuffer().getNumChannels() : 2;
        const double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;
        document.newEmptyDocument(numCh, sr);

        if (onSourceNameChanged) onSourceNameChanged({});
        if (onStatusMessage) onStatusMessage("Cleared");
    };
    autoRecordButton.onClick = [this]
    {
        document.autoRecordEnabled = autoRecordButton.getToggleState();
        if (! document.autoRecordEnabled)
            document.autoRecordTriggered = false;   // cancelling standby clears any late trigger
        document.notifyChanged();
    };

    applyTheme();
    document.changeBroadcaster.addChangeListener(this);
    theme->addChangeListener(this);
    updateTransportButtonText();
    startTimerHz(15);
}

EditorToolbar::~EditorToolbar()
{
    for (auto* b : { &playFromStartButton, &playButton, &loopButton, &scrubButton, &recordButton,
                     &toolsButton, &reverseButton, &clearButton, &autoRecordButton })
        b->setLookAndFeel(nullptr);   // detach before toolbarLnF is destroyed
    theme->removeChangeListener(this);
    document.changeBroadcaster.removeChangeListener(this);
}

void EditorToolbar::applyTheme()
{
    const auto& pal = theme->palette();

    // Play and Record are always filled (primary actions); Loop fills only when on;
    // Tools and Play-from-start stay outlined -- the latter is a secondary/modifier
    // action next to Play, not a primary one. A fully transparent buttonColourId is
    // R3WRKLookAndFeel's cue to draw the outline style instead of a solid pill --
    // see drawButtonBackground().
    playButton.setColour(juce::TextButton::buttonColourId, pal.accent);
    playButton.setColour(juce::TextButton::textColourOffId, pal.windowBg);

    playFromStartButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    playFromStartButton.setColour(juce::TextButton::textColourOffId, pal.screenText);

    // Loop/Tools are outlined and Play/Record are filled, but all four now sit on the dark
    // control band (paint() fills panelBg behind them), so their text -- like WaveformDisplay
    // and TimeRuler -- comes from screenText/screenTextDim rather than text/textDim.
    loopButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    loopButton.setColour(juce::TextButton::buttonOnColourId, pal.accent);
    loopButton.setColour(juce::TextButton::textColourOffId, pal.screenText);
    loopButton.setColour(juce::TextButton::textColourOnId, pal.windowBg);

    // Scrub is a toggling tool, same treatment as Loop: outlined while off, filled while the
    // tool is selected.
    scrubButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    scrubButton.setColour(juce::TextButton::buttonOnColourId, pal.accent);
    scrubButton.setColour(juce::TextButton::textColourOffId, pal.screenText);
    scrubButton.setColour(juce::TextButton::textColourOnId, pal.windowBg);

    recordButton.setColour(juce::TextButton::buttonColourId, pal.recordButton);
    recordButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);   // the stop-square ink while recording

    toolsButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    toolsButton.setColour(juce::TextButton::textColourOffId, pal.screenText);

    // Reverse runs immediately (no on/off state of its own), same outlined treatment as
    // Tools/Play-from-start.
    reverseButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    reverseButton.setColour(juce::TextButton::textColourOffId, pal.screenText);

    // Clear discards audio, so its X is inked in the same red as the record button rather
    // than the neutral screenText every other outlined icon uses -- a quiet "careful, this
    // one's destructive" cue, still just an outline rather than a filled warning circle.
    clearButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    clearButton.setColour(juce::TextButton::textColourOffId, pal.recordButton);

    // Auto-Record is a toggling tool, same treatment as Loop/Scrub: outlined while off,
    // filled while armed and waiting for the input to cross the threshold.
    autoRecordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    autoRecordButton.setColour(juce::TextButton::buttonOnColourId, pal.accent);
    autoRecordButton.setColour(juce::TextButton::textColourOffId, pal.screenText);
    autoRecordButton.setColour(juce::TextButton::textColourOnId, pal.windowBg);

    // timeLabel's colour flips to pal.playhead while recording -- see timerCallback().
    timeLabel.setColour(juce::Label::textColourId, pal.screenTextDim);
    repaint();
}

void EditorToolbar::paint(juce::Graphics& g)
{
    // The control band: a rounded dark panel (the same "screen" tone as the waveform and
    // ruler) behind the whole transport row.
    g.setColour(theme->palette().panelBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
}

//==============================================================================
void EditorToolbar::doCut()   { EditActions::cut(document, clipboard); }
void EditorToolbar::doCopy()  { EditActions::copy(document, clipboard); }
void EditorToolbar::doPaste() { EditActions::pasteReplace(document, clipboard); }
void EditorToolbar::doUndo()  { document.undoManager.undo(); document.notifyChanged(); }
void EditorToolbar::doRedo()  { document.undoManager.redo(); document.notifyChanged(); }

void EditorToolbar::toggleTransport()
{
    if (document.isRecording.load())      { processor.stopRecording(); autoSaveRecording(); }
    else if (document.isPlaying.load())   processor.stopPlayback();
    else                                { processor.startRecording();
                                          if (onSourceNameChanged) onSourceNameChanged({}); }
    updateTransportButtonText();
}

void EditorToolbar::togglePlay()
{
    if (document.isRecording.load())    { processor.stopRecording(); autoSaveRecording(); }
    else if (document.isPlaying.load()) processor.stopPlayback();
    else                               processor.startPlayback();
    updateTransportButtonText();
}

void EditorToolbar::playFromStart()
{
    if (document.isRecording.load())
    {
        processor.stopRecording();
        autoSaveRecording();
    }
    else
    {
        if (document.isPlaying.load())
            processor.stopPlayback();

        // Force the playhead to sample 0 and (re)start -- startPlayback()/processBlock
        // then snap it into whatever's actually playing (the selection, else the loop
        // range, else the whole clip), so this always lands at the start of that region,
        // not necessarily the document's literal first sample.
        document.playhead = 0;
        processor.startPlayback();
    }
    updateTransportButtonText();
}

void EditorToolbar::autoSaveRecording()
{
    if (document.isEmpty())
        return;

    const auto file = outputSettings->makeWavFile(false);
    if (document.saveToFile(file))
    {
        if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
        if (onSaved)             onSaved();
        if (onStatusMessage)     onStatusMessage("Saved " + file.getFileName());
    }
    else if (onStatusMessage)
    {
        onStatusMessage("Couldn't save to " + file.getParentDirectory().getFileName());
    }
}

void EditorToolbar::revertToOriginal()
{
    document.revertToOriginal();
}

//==============================================================================
void EditorToolbar::showToolsMenu()
{
    enum { idOpen = 1, idSave, idRevert,
           idCut, idCopy, idPaste,
           idTrim, idDelete, idSilence,
           idNormalize, idAmplify, idFadeIn, idFadeOut, idReverse,
           idStretch, idExportSel,
           idOutputFolder, idTheme, idAutoRecordThreshold,
           idUndo, idRedo };

    const bool empty   = document.isEmpty();
    const bool sel     = document.hasSelection();
    const bool clip    = clipboard.hasContent();
    const bool canUndo = document.undoManager.canUndo();
    const bool canRedo = document.undoManager.canRedo();

    const juce::String cmd   = juce::String::fromUTF8("\xe2\x8c\x98");           // ⌘
    const juce::String shift = juce::String::fromUTF8("\xe2\x87\xa7");           // ⇧
    auto keyed = [](juce::String text, int id, bool enabled, juce::String shortcut)
    {
        juce::PopupMenu::Item i(std::move(text));
        i.itemID = id; i.isEnabled = enabled; i.shortcutKeyDescription = std::move(shortcut);
        return i;
    };

    juce::PopupMenu m;
    m.addItem(idOpen,   juce::String::fromUTF8("Open\xE2\x80\xA6"));
    m.addItem(idSave,   juce::String::fromUTF8("Save As\xE2\x80\xA6"), ! empty);
    m.addItem(idRevert, "Revert to Original", ! empty);
    m.addSeparator();
    m.addItem(keyed("Cut",   idCut,   sel,  cmd + "X"));
    m.addItem(keyed("Copy",  idCopy,  sel,  cmd + "C"));
    m.addItem(keyed("Paste", idPaste, clip, cmd + "V"));
    m.addSeparator();
    m.addItem(keyed("Trim to Selection", idTrim, sel, cmd + "T"));
    m.addItem(idDelete,  "Delete Selection",  sel);
    m.addItem(idSilence, "Silence Selection", ! empty);
    m.addSeparator();
    m.addItem(idNormalize, "Normalize",  ! empty);
    m.addItem(idAmplify,   juce::String::fromUTF8("Amplify\xE2\x80\xA6"), ! empty);
    m.addItem(idFadeIn,    "Fade In",    ! empty);
    m.addItem(idFadeOut,   "Fade Out",   ! empty);
    m.addItem(idReverse,   "Reverse",    ! empty);
    m.addSeparator();
    m.addItem(idStretch,   juce::String::fromUTF8("Stretch / Pitch\xE2\x80\xA6"), ! empty);
    m.addItem(idExportSel, "Export Selection to Folder", sel);
    m.addSeparator();
    m.addItem(idOutputFolder, juce::String::fromUTF8("Output Folder\xE2\x80\xA6"));
    m.addItem(idTheme,        juce::String::fromUTF8("Theme\xE2\x80\xA6"));
    m.addItem(idAutoRecordThreshold, juce::String::fromUTF8("Auto-Record Threshold\xE2\x80\xA6"));
    m.addSeparator();
    m.addItem(keyed("Undo", idUndo, canUndo, cmd + "Z"));
    m.addItem(keyed("Redo", idRedo, canRedo, shift + cmd + "Z"));

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolsButton), [this](int r)
    {
        switch (r)
        {
            case idOpen:      openFile();   break;
            case idSave:      saveFile();   break;
            case idRevert:    revertToOriginal(); break;
            case idCut:       doCut();      break;
            case idCopy:      doCopy();     break;
            case idPaste:     doPaste();    break;
            case idTrim:      EditActions::trimToSelection(document); break;
            case idDelete:    EditActions::deleteSelection(document); break;
            case idSilence:   EditActions::silence(document);        break;
            case idNormalize: EditActions::normalize(document);      break;
            case idAmplify:   showAmplifyCallout(toolsButton.getScreenBounds()); break;
            case idFadeIn:    EditActions::fadeIn(document);  break;
            case idFadeOut:   EditActions::fadeOut(document); break;
            case idReverse:   EditActions::reverse(document); break;
            case idStretch:      showStretchCallout(toolsButton.getScreenBounds()); break;
            case idExportSel:    exportSelectionToFolder(); break;
            case idOutputFolder: chooseOutputFolder();      break;
            case idTheme:        showThemeCallout();        break;
            case idAutoRecordThreshold: showAutoRecordThresholdCallout(); break;
            case idUndo:      doUndo(); break;
            case idRedo:      doRedo(); break;
            default: break;
        }
    });
}

void EditorToolbar::showAmplifyCallout(juce::Rectangle<int> screenTargetArea)
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<AmplifyPanel>(document),
                                           screenTargetArea, nullptr);
}

void EditorToolbar::showStretchCallout(juce::Rectangle<int> screenTargetArea)
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<StretchPanel>(document),
                                           screenTargetArea, nullptr);
}

//==============================================================================
void EditorToolbar::showSelectionContextMenu(juce::Point<int> screenPosition)
{
    enum { idTrim = 1, idAmplify, idReverse, idStretch };

    const juce::String cmd = juce::String::fromUTF8("\xe2\x8c\x98");   // ⌘
    juce::PopupMenu::Item trimItem("Trim to Selection");
    trimItem.itemID = idTrim;
    trimItem.shortcutKeyDescription = cmd + "T";

    juce::PopupMenu m;
    m.addItem(trimItem);
    m.addItem(idAmplify, juce::String::fromUTF8("Amplify\xE2\x80\xA6"));
    m.addItem(idReverse, "Reverse");
    m.addItem(idStretch, juce::String::fromUTF8("Stretch / Pitch\xE2\x80\xA6"));

    const auto targetArea = juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea), [this, targetArea](int r)
    {
        switch (r)
        {
            case idTrim:    EditActions::trimToSelection(document); break;
            case idAmplify: showAmplifyCallout(targetArea); break;
            case idReverse: EditActions::reverse(document); break;
            case idStretch: showStretchCallout(targetArea); break;
            default: break;
        }
    });
}

void EditorToolbar::doTrim() { EditActions::trimToSelection(document); }

void EditorToolbar::showThemeCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<ThemeEditor>(),
                                           toolsButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showAutoRecordThresholdCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<AutoRecordThresholdPanel>(document),
                                           toolsButton.getScreenBounds(), nullptr);
}

//==============================================================================
void EditorToolbar::updateTransportButtonText()
{
    const bool rec = document.isRecording.load();
    const bool playing = document.isPlaying.load();
    recordButton.setButtonText(rec ? R3WRKLookAndFeel::iconStop : juce::String());
    playButton.setButtonText(playing ? R3WRKLookAndFeel::iconStop : R3WRKLookAndFeel::iconPlay);
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
    recordButton.setEnabled(! playing || rec);
    scrubButton.setEnabled(! rec);
    reverseButton.setEnabled(! rec);
    clearButton.setEnabled(! rec);
    autoRecordButton.setEnabled(! rec);
}

void EditorToolbar::timerCallback()
{
    // Auto-Record standby: the audio thread only ever flips this atomic when the input
    // crosses the threshold -- it can't safely allocate/touch document or transport state
    // itself (see PluginProcessor::processBlock's idle branch), so this timer is where the
    // real start happens, on the message thread, the same handoff every other audio-thread ->
    // UI-facing state change in this class already uses.
    if (document.autoRecordTriggered.load())
    {
        document.autoRecordTriggered = false;
        document.autoRecordEnabled = false;
        autoRecordButton.setToggleState(false, juce::dontSendNotification);
        processor.startRecording();
    }

    updateTransportButtonText();

    const double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;

    if (document.isRecording.load())
    {
        const double recSec = (double) document.recordedSamples.load() / sr;
        timeLabel.setColour(juce::Label::textColourId, theme->palette().playhead);
        timeLabel.setText(juce::String::fromUTF8("\xE2\x97\x8F REC  ") + mmss(recSec), juce::dontSendNotification);
    }
    else
    {
        const double posSec = (double) document.playhead.load() / sr;
        const double lenSec = (double) document.getNumSamples() / sr;
        timeLabel.setColour(juce::Label::textColourId, theme->palette().screenTextDim);
        timeLabel.setText(formatTime(posSec) + " / " + formatTime(lenSec), juce::dontSendNotification);
    }
}

void EditorToolbar::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &theme.get())
    {
        applyTheme();
        return;
    }
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
}

//==============================================================================
void EditorToolbar::openFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Open audio file", juce::File(),
                                                      "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3");
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file.existsAsFile())
            loadAudioFile(file);
    });
}

void EditorToolbar::loadAudioFile(const juce::File& file)
{
    if (document.loadFromFile(file, processor.getSampleRate()))
    {
        if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
        if (onSaved) onSaved();
    }
}

void EditorToolbar::saveFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Save audio as WAV",
                                                     outputSettings->folder(), "*.wav");
    auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
               | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File() && document.saveToFile(file))
        {
            if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
            if (onSaved) onSaved();
            if (onStatusMessage) onStatusMessage("Saved " + file.getFileName());
        }
    });
}

void EditorToolbar::exportSelectionToFolder()
{
    if (! document.hasSelection())
        return;

    const auto file = outputSettings->makeWavFile(true);
    if (EditActions::exportSelection(document, file))
    {
        if (onStatusMessage) onStatusMessage("Exported " + file.getFileName());
    }
    else if (onStatusMessage)
    {
        onStatusMessage("Couldn't export to " + file.getParentDirectory().getFileName());
    }
}

void EditorToolbar::chooseOutputFolder()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose the output folder for recordings and exports",
                                                     outputSettings->folder());
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto dir = fc.getResult();
        if (dir.isDirectory())
        {
            outputSettings->setFolder(dir);
            if (onStatusMessage) onStatusMessage("Output folder: " + dir.getFileName());
        }
    });
}

//==============================================================================
void EditorToolbar::resized()
{
    auto row = getLocalBounds().reduced(8, 5);   // inset so pills clear the band's rounded corners
    const int gap = 16;   // trying wider spacing between the transport buttons

    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;
    auto add = [&](juce::Component& c, int w)
    {
        fb.items.add(juce::FlexItem(c).withWidth((float) w).withMinWidth(22.0f)
                         .withMargin(juce::FlexItem::Margin(0, (float) gap, 0, 0)));
    };
    // Left-grouped, matching the mockup: Play/Loop/Record/Tools together, time pinned right.
    // Auto-Record sits right after Record (a Record modifier, at the user's request); Scrub/
    // Reverse/Clear sit last, at the right-hand end of the button cluster -- round like every
    // other icon button here (the reel-hub icon reads better in a circle than the earlier
    // rectangular cassette-body version did).
    add(playFromStartButton, 28);
    add(playButton, 28);
    add(loopButton, 28);
    add(recordButton, 28);
    add(autoRecordButton, 28);
    add(toolsButton, 28);
    add(scrubButton, 28);
    add(reverseButton, 28);
    add(clearButton, 28);
    fb.items.add(juce::FlexItem().withFlex(1.0f));
    add(timeLabel, 150);
    fb.performLayout(row);
}
