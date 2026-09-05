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
}

//==============================================================================
EditorToolbar::EditorToolbar(R3WRKAudioProcessor& proc, AudioDocument& doc)
    : processor(proc), document(doc)
{
    addAndMakeVisible(playButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(timeLabel);
    addAndMakeVisible(recordButton);
    addAndMakeVisible(toolsButton);

    timeLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    timeLabel.setJustificationType(juce::Justification::centredRight);
    toolsButton.setButtonText(juce::String::fromUTF8("Tools \xe2\x96\xbe"));   // Tools ▾
    loopButton.setClickingTogglesState(true);

    for (auto* b : { &playButton, &loopButton, &recordButton, &toolsButton })
        b->setLookAndFeel(&toolbarLnF);

    recordButton.onClick = [this] { toggleTransport(); };
    playButton.onClick   = [this] { togglePlay(); };
    loopButton.onClick   = [this] { document.loopEnabled = loopButton.getToggleState(); };
    toolsButton.onClick  = [this] { showToolsMenu(); };

    applyTheme();
    document.changeBroadcaster.addChangeListener(this);
    theme->addChangeListener(this);
    updateTransportButtonText();
    startTimerHz(15);
}

EditorToolbar::~EditorToolbar()
{
    for (auto* b : { &playButton, &loopButton, &recordButton, &toolsButton })
        b->setLookAndFeel(nullptr);   // detach before toolbarLnF is destroyed
    theme->removeChangeListener(this);
    document.changeBroadcaster.removeChangeListener(this);
}

void EditorToolbar::applyTheme()
{
    const auto& pal = theme->palette();

    // Play and Record are always filled (primary actions); Loop fills only when on;
    // Tools stays outlined. A fully transparent buttonColourId is R3WRKLookAndFeel's cue
    // to draw the outline style instead of a solid pill -- see drawButtonBackground().
    playButton.setColour(juce::TextButton::buttonColourId, pal.accent);
    playButton.setColour(juce::TextButton::textColourOffId, pal.windowBg);

    // Loop/Tools are outlined and Play/Record are filled, but all four now sit on the dark
    // control band (paint() fills panelBg behind them), so their text -- like WaveformDisplay
    // and TimeRuler -- comes from screenText/screenTextDim rather than text/textDim.
    loopButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    loopButton.setColour(juce::TextButton::buttonOnColourId, pal.accent);
    loopButton.setColour(juce::TextButton::textColourOffId, pal.screenText);
    loopButton.setColour(juce::TextButton::textColourOnId, pal.windowBg);

    recordButton.setColour(juce::TextButton::buttonColourId, pal.recordButton);

    toolsButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    toolsButton.setColour(juce::TextButton::textColourOffId, pal.screenText);

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

void EditorToolbar::revertAll()
{
    while (document.undoManager.canUndo())
        document.undoManager.undo();
    document.notifyChanged();
}

//==============================================================================
void EditorToolbar::showToolsMenu()
{
    enum { idOpen = 1, idSave, idRevert,
           idCut, idCopy, idPaste,
           idTrim, idDelete, idSilence,
           idNormalize, idAmplify, idFadeIn, idFadeOut, idReverse,
           idStretch, idExportSel,
           idOutputFolder, idTheme,
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
    m.addItem(idRevert, "Revert", canUndo);
    m.addSeparator();
    m.addItem(keyed("Cut",   idCut,   sel,  cmd + "X"));
    m.addItem(keyed("Copy",  idCopy,  sel,  cmd + "C"));
    m.addItem(keyed("Paste", idPaste, clip, cmd + "V"));
    m.addSeparator();
    m.addItem(idTrim,    "Trim to Selection", sel);
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
    m.addSeparator();
    m.addItem(keyed("Undo", idUndo, canUndo, cmd + "Z"));
    m.addItem(keyed("Redo", idRedo, canRedo, shift + cmd + "Z"));

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolsButton), [this](int r)
    {
        switch (r)
        {
            case idOpen:      openFile();   break;
            case idSave:      saveFile();   break;
            case idRevert:    revertAll();  break;
            case idCut:       doCut();      break;
            case idCopy:      doCopy();     break;
            case idPaste:     doPaste();    break;
            case idTrim:      EditActions::trimToSelection(document); break;
            case idDelete:    EditActions::deleteSelection(document); break;
            case idSilence:   EditActions::silence(document);        break;
            case idNormalize: EditActions::normalize(document);      break;
            case idAmplify:   showAmplifyCallout(); break;
            case idFadeIn:    EditActions::fadeIn(document);  break;
            case idFadeOut:   EditActions::fadeOut(document); break;
            case idReverse:   EditActions::reverse(document); break;
            case idStretch:      showStretchCallout();      break;
            case idExportSel:    exportSelectionToFolder(); break;
            case idOutputFolder: chooseOutputFolder();      break;
            case idTheme:        showThemeCallout();        break;
            case idUndo:      doUndo(); break;
            case idRedo:      doRedo(); break;
            default: break;
        }
    });
}

void EditorToolbar::showAmplifyCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<AmplifyPanel>(document),
                                           toolsButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showStretchCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<StretchPanel>(document),
                                           toolsButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showThemeCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<ThemeEditor>(),
                                           toolsButton.getScreenBounds(), nullptr);
}

//==============================================================================
void EditorToolbar::updateTransportButtonText()
{
    const bool rec = document.isRecording.load();
    const bool playing = document.isPlaying.load();
    recordButton.setButtonText(rec ? "Stop Rec" : "Record");
    playButton.setButtonText(playing ? "Stop" : "Play");
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
    recordButton.setEnabled(! playing || rec);
}

void EditorToolbar::timerCallback()
{
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
        if (file.existsAsFile() && document.loadFromFile(file, processor.getSampleRate()))
        {
            if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
            if (onSaved) onSaved();
        }
    });
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
    const int gap = 4;

    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;
    auto add = [&](juce::Component& c, int w)
    {
        fb.items.add(juce::FlexItem(c).withWidth((float) w).withMinWidth(22.0f)
                         .withMargin(juce::FlexItem::Margin(0, (float) gap, 0, 0)));
    };
    // Left-grouped, matching the mockup: Play/Loop/Record/Tools together, time pinned right.
    add(playButton, 58);
    add(loopButton, 56);
    add(recordButton, 80);
    add(toolsButton, 84);
    fb.items.add(juce::FlexItem().withFlex(1.0f));
    add(timeLabel, 150);
    fb.performLayout(row);
}
