#include "EditorToolbar.h"
#include "TimeStretchEngine.h"

namespace
{
    juce::String formatTime(double seconds)
    {
        int mins = (int) (seconds / 60.0);
        double secs = seconds - mins * 60.0;
        return juce::String::formatted("%02d:%06.3f", mins, secs);
    }

    void dismissEnclosingCallout(juce::Component& c)
    {
        if (auto* box = c.findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    //==============================================================================
    struct GainPanel : juce::Component
    {
        explicit GainPanel(AudioDocument& doc) : document(doc)
        {
            title.setText("Gain", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            gain.setRange(-24.0, 24.0, 0.1);
            gain.setValue(0.0);
            gain.setTextValueSuffix(" dB");
            gain.setSliderStyle(juce::Slider::LinearHorizontal);
            gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);
            apply.onClick = [this]
            {
                EditActions::applyGainDb(document, (float) gain.getValue());
                dismissEnclosingCallout(*this);
            };
            addAndMakeVisible(title);
            addAndMakeVisible(gain);
            addAndMakeVisible(apply);
            setSize(260, 84);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(18));
            r.removeFromTop(4);
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

            stretchLabel.setText("Length", juce::dontSendNotification);
            stretch.setRange(0.25, 4.0, 0.01);
            stretch.setValue(1.0);
            stretch.setSkewFactorFromMidPoint(1.0);
            stretch.setTextValueSuffix(" x");
            stretch.setSliderStyle(juce::Slider::LinearHorizontal);
            stretch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);

            pitchLabel.setText("Pitch", juce::dontSendNotification);
            pitch.setRange(-24.0, 24.0, 0.1);
            pitch.setValue(0.0);
            pitch.setTextValueSuffix(" st");
            pitch.setSliderStyle(juce::Slider::LinearHorizontal);
            pitch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);

            hint.setText("Applies to the selection, or the whole clip if nothing is selected.",
                         juce::dontSendNotification);
            hint.setFont(juce::FontOptions(11.0f));
            hint.setColour(juce::Label::textColourId, juce::Colours::grey);

            apply.onClick = [this] { run(); dismissEnclosingCallout(*this); };

            addAndMakeVisible(title);
            addAndMakeVisible(stretchLabel);
            addAndMakeVisible(stretch);
            addAndMakeVisible(pitchLabel);
            addAndMakeVisible(pitch);
            addAndMakeVisible(hint);
            addAndMakeVisible(apply);
            setSize(320, 150);
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
            stretchLabel.setBounds(row.removeFromLeft(46));
            stretch.setBounds(row);
            r.removeFromTop(6);
            row = r.removeFromTop(24);
            pitchLabel.setBounds(row.removeFromLeft(46));
            pitch.setBounds(row);
            r.removeFromTop(8);
            apply.setBounds(r.removeFromTop(24).removeFromRight(72));
            r.removeFromTop(4);
            hint.setBounds(r);
        }

        AudioDocument& document;
        juce::Label title, stretchLabel, pitchLabel, hint;
        juce::Slider stretch, pitch;
        juce::TextButton apply { "Apply" };
    };

    //==============================================================================
    struct ChopPanel : juce::Component
    {
        ChopPanel(AudioDocument& doc, std::function<void()> exportSlices)
            : document(doc), onExport(std::move(exportSlices))
        {
            title.setText("Chop to Grid", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));

            bpmLabel.setText("BPM", juce::dontSendNotification);
            bpm.setRange(40.0, 240.0, 0.1);
            bpm.setValue(document.chopBpm);
            bpm.setSliderStyle(juce::Slider::LinearHorizontal);
            bpm.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);
            bpm.onValueChange = [this] { push(); };

            divLabel.setText("Grid", juce::dontSendNotification);
            division.addItem("1/4", 4);
            division.addItem("1/8", 8);
            division.addItem("1/16", 16);
            division.addItem("1/32", 32);
            division.setSelectedId(document.chopDivision > 0 ? document.chopDivision : 16,
                                   juce::dontSendNotification);
            division.onChange = [this] { push(); };

            update.onClick = [this] { push(); document.recalculateChopMarkers(); document.notifyChanged(); };
            exportBtn.onClick = [this]
            {
                push();
                if (onExport) onExport();
                dismissEnclosingCallout(*this);
            };

            addAndMakeVisible(title);
            addAndMakeVisible(bpmLabel);
            addAndMakeVisible(bpm);
            addAndMakeVisible(divLabel);
            addAndMakeVisible(division);
            addAndMakeVisible(update);
            addAndMakeVisible(exportBtn);
            setSize(300, 132);
        }

        void push()
        {
            document.chopBpm = bpm.getValue();
            document.chopDivision = division.getSelectedId();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(18));
            r.removeFromTop(6);
            auto row = r.removeFromTop(24);
            bpmLabel.setBounds(row.removeFromLeft(40));
            bpm.setBounds(row);
            r.removeFromTop(6);
            row = r.removeFromTop(24);
            divLabel.setBounds(row.removeFromLeft(40));
            division.setBounds(row.removeFromLeft(90));
            r.removeFromTop(8);
            row = r.removeFromTop(24);
            update.setBounds(row.removeFromLeft(96));
            row.removeFromLeft(6);
            exportBtn.setBounds(row);
        }

        AudioDocument& document;
        std::function<void()> onExport;
        juce::Label title, bpmLabel, divLabel;
        juce::Slider bpm;
        juce::ComboBox division;
        juce::TextButton update { "Update Grid" }, exportBtn { "Export Slices\xE2\x80\xA6" };
    };
}

//==============================================================================
EditorToolbar::EditorToolbar(EdisonCloneAudioProcessor& proc, AudioDocument& doc)
    : processor(proc), document(doc)
{
    for (auto* b : { &recordButton, &playButton, &openButton, &saveButton, &undoButton, &redoButton,
                     &viewButton, &zoomFitButton, &editMenuButton })
        addAndMakeVisible(b);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(statusLabel);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    statusLabel.setJustificationType(juce::Justification::centredRight);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    viewButton.setTooltip("Switch between the waveform and spectrogram view");

    recordButton.onClick = [this] { toggleTransport(); };
    playButton.onClick   = [this] { togglePlay(); };
    loopButton.onClick = [this] { document.loopEnabled = loopButton.getToggleState(); };

    openButton.onClick = [this] { openFile(); };
    saveButton.onClick = [this] { saveFile(); };
    undoButton.onClick = [this] { doUndo(); };
    redoButton.onClick = [this] { doRedo(); };

    viewButton.onClick = [this]
    {
        showingSpectrogram = ! showingSpectrogram;
        viewButton.setButtonText(showingSpectrogram ? "Waveform" : "Spectrogram");
        if (onViewModeChanged) onViewModeChanged(showingSpectrogram);
    };
    zoomFitButton.onClick = [this] { if (onZoomFit) onZoomFit(); };

    editMenuButton.onClick = [this] { showEditMenu(); };

    document.changeBroadcaster.addChangeListener(this);
    updateTransportButtonText();
    startTimerHz(15);
}

EditorToolbar::~EditorToolbar()
{
    document.changeBroadcaster.removeChangeListener(this);
}

//==============================================================================
void EditorToolbar::doCut()   { EditActions::cut(document, clipboard); }
void EditorToolbar::doCopy()  { EditActions::copy(document, clipboard); }
void EditorToolbar::doPaste() { EditActions::pasteReplace(document, clipboard); }
void EditorToolbar::doUndo()  { document.undoManager.undo(); document.notifyChanged(); }
void EditorToolbar::doRedo()  { document.undoManager.redo(); document.notifyChanged(); }

void EditorToolbar::toggleTransport()
{
    if (document.isRecording.load())      processor.stopRecording();
    else if (document.isPlaying.load())   processor.stopPlayback();
    else                                  processor.startRecording();
    updateTransportButtonText();
}

void EditorToolbar::togglePlay()
{
    if (document.isRecording.load())    processor.stopRecording();
    else if (document.isPlaying.load()) processor.stopPlayback();
    else                               processor.startPlayback();
    updateTransportButtonText();
}

//==============================================================================
void EditorToolbar::showEditMenu()
{
    enum
    {
        idCut = 1, idCopy, idPaste,
        idTrim, idDelete, idSilence,
        idNormalize, idGain, idFadeIn, idFadeOut, idReverse,
        idStretch, idChop,
        idLoopToSel,
        idZoomIn, idZoomOut
    };

    const bool empty = document.isEmpty();
    const bool haveClip = clipboard.hasContent();

    const juce::String cmdSym = juce::String::fromUTF8("\xe2\x8c\x98");   // ⌘

    juce::PopupMenu m;
    juce::PopupMenu::Item cut ("Cut");     cut.itemID = idCut;     cut.isEnabled = ! empty;   cut.shortcutKeyDescription = cmdSym + "X";
    juce::PopupMenu::Item copy ("Copy");   copy.itemID = idCopy;   copy.isEnabled = ! empty;  copy.shortcutKeyDescription = cmdSym + "C";
    juce::PopupMenu::Item paste ("Paste"); paste.itemID = idPaste; paste.isEnabled = haveClip; paste.shortcutKeyDescription = cmdSym + "V";
    m.addItem(cut); m.addItem(copy); m.addItem(paste);
    m.addSeparator();
    m.addItem(idTrim,      "Trim to Selection", ! empty);
    m.addItem(idDelete,    "Delete Selection",  ! empty);
    m.addItem(idSilence,   "Silence Selection", ! empty);
    m.addSeparator();
    m.addItem(idNormalize, "Normalize",         ! empty);
    m.addItem(idGain,      juce::String::fromUTF8("Gain\xE2\x80\xA6"), ! empty);
    m.addItem(idFadeIn,    "Fade In",           ! empty);
    m.addItem(idFadeOut,   "Fade Out",          ! empty);
    m.addItem(idReverse,   "Reverse",           ! empty);
    m.addSeparator();
    m.addItem(idStretch,   juce::String::fromUTF8("Stretch / Pitch\xE2\x80\xA6"), ! empty);
    m.addItem(idChop,      juce::String::fromUTF8("Chop to Grid\xE2\x80\xA6"),    ! empty);
    m.addSeparator();
    m.addItem(idLoopToSel, "Set Loop to Selection", ! empty);
    m.addSeparator();
    m.addItem(idZoomIn,  "Zoom In");
    m.addItem(idZoomOut, "Zoom Out");

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(editMenuButton),
                    [this](int r)
    {
        switch (r)
        {
            case idCut:       doCut(); break;
            case idCopy:      doCopy(); break;
            case idPaste:     doPaste(); break;
            case idTrim:      EditActions::trimToSelection(document); break;
            case idDelete:    EditActions::deleteSelection(document); break;
            case idSilence:   EditActions::silence(document); break;
            case idNormalize: EditActions::normalize(document); break;
            case idGain:      showGainCallout(); break;
            case idFadeIn:    EditActions::fadeIn(document); break;
            case idFadeOut:   EditActions::fadeOut(document); break;
            case idReverse:   EditActions::reverse(document); break;
            case idStretch:   showStretchCallout(); break;
            case idChop:      showChopCallout(); break;
            case idLoopToSel:
                if (document.hasSelection())
                {
                    document.loopStart = document.getSelectionStart();
                    document.loopEnd   = document.getSelectionEnd();
                    document.notifyChanged();
                }
                break;
            case idZoomIn:    if (onZoomIn)  onZoomIn();  break;
            case idZoomOut:   if (onZoomOut) onZoomOut(); break;
            default: break;
        }
    });
}

void EditorToolbar::showGainCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<GainPanel>(document),
                                           editMenuButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showStretchCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<StretchPanel>(document),
                                           editMenuButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showChopCallout()
{
    juce::CallOutBox::launchAsynchronously(
        std::make_unique<ChopPanel>(document, [this] { exportSlices(); }),
        editMenuButton.getScreenBounds(), nullptr);
}

//==============================================================================
void EditorToolbar::updateTransportButtonText()
{
    recordButton.setButtonText(document.isRecording.load() ? "Stop Rec" : "Record");
    playButton.setButtonText(document.isPlaying.load() ? "Stop" : "Play");
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
    recordButton.setEnabled(! document.isPlaying.load() || document.isRecording.load());
}

void EditorToolbar::timerCallback()
{
    updateTransportButtonText();

    double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;
    double posSec = (double) document.playhead.load() / sr;
    double lenSec = (double) document.getNumSamples() / sr;
    juce::String status = formatTime(posSec) + " / " + formatTime(lenSec) + "   (" + juce::String((int) sr) + " Hz)";
    if (document.hasSelection())
    {
        double selSec = (double) (document.getSelectionEnd() - document.getSelectionStart()) / sr;
        status << "   Sel: " << juce::String(selSec, 3) << "s";
    }
    if (document.isRecording.load())
        status = "RECORDING   " + status;
    statusLabel.setText(status, juce::dontSendNotification);
}

void EditorToolbar::changeListenerCallback(juce::ChangeBroadcaster*)
{
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
            document.loadFromFile(file, processor.getSampleRate());
    });
}

void EditorToolbar::saveFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Save audio as WAV", juce::File(), "*.wav");
    auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
               | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File())
            document.saveToFile(file);
    });
}

void EditorToolbar::exportSlices()
{
    document.recalculateChopMarkers();
    document.notifyChanged();

    fileChooser = std::make_unique<juce::FileChooser>("Choose a folder for the exported slices", juce::File());
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto folder = fc.getResult();
        if (folder.isDirectory())
            EditActions::exportChopSlices(document, folder, "slice");
    });
}

//==============================================================================
void EditorToolbar::resized()
{
    auto area = getLocalBounds().reduced(4);
    const int gap = 4;

    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;
    fb.alignItems = juce::FlexBox::AlignItems::stretch;

    auto add = [&](juce::Component& c, int w)
    {
        fb.items.add(juce::FlexItem(c).withWidth((float) w)
                        .withMargin(juce::FlexItem::Margin(0, gap, 0, 0)));
    };
    auto spacer = [&](int w) { fb.items.add(juce::FlexItem().withWidth((float) w)); };

    add(recordButton, 76);
    add(playButton, 58);
    add(loopButton, 58);
    spacer(8);
    add(openButton, 62);
    add(saveButton, 62);
    spacer(8);
    add(undoButton, 56);
    add(redoButton, 56);
    spacer(8);
    add(viewButton, 100);
    add(zoomFitButton, 46);
    spacer(8);
    add(editMenuButton, 72);
    fb.items.add(juce::FlexItem(statusLabel).withFlex(1.0f));

    fb.performLayout(area.removeFromTop(28));
}
