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
            bpm.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
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
    for (auto* b : { &recordButton, &playButton,
                     &trimButton, &deleteButton, &silenceButton, &normalizeButton, &amplifyButton,
                     &reverseButton, &fadeInButton, &fadeOutButton,
                     &cutButton, &copyButton, &pasteButton, &undoButton, &redoButton,
                     &openButton, &saveButton, &revertButton, &toolsButton })
        addAndMakeVisible(b);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(timeLabel);
    addAndMakeVisible(recLabel);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    timeLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    timeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    recLabel.setJustificationType(juce::Justification::centredRight);
    recLabel.setColour(juce::Label::textColourId, juce::Colours::red);
    undoButton.setButtonText(juce::String::fromUTF8("\xe2\x86\xb6"));   // ↶
    redoButton.setButtonText(juce::String::fromUTF8("\xe2\x86\xb7"));   // ↷
    undoButton.setTooltip("Undo");
    redoButton.setTooltip("Redo");
    toolsButton.setButtonText(juce::String::fromUTF8("Tools \xe2\x96\xbe"));   // Tools ▾

    recordButton.onClick = [this] { toggleTransport(); };
    playButton.onClick   = [this] { togglePlay(); };
    loopButton.onClick   = [this] { document.loopEnabled = loopButton.getToggleState(); };

    trimButton.onClick      = [this] { EditActions::trimToSelection(document); };
    deleteButton.onClick    = [this] { EditActions::deleteSelection(document); };
    silenceButton.onClick   = [this] { EditActions::silence(document); };
    normalizeButton.onClick = [this] { EditActions::normalize(document); };
    amplifyButton.onClick   = [this] { showAmplifyCallout(); };
    reverseButton.onClick   = [this] { EditActions::reverse(document); };
    fadeInButton.onClick    = [this] { EditActions::fadeIn(document); };
    fadeOutButton.onClick   = [this] { EditActions::fadeOut(document); };
    cutButton.onClick       = [this] { doCut(); };
    copyButton.onClick      = [this] { doCopy(); };
    pasteButton.onClick     = [this] { doPaste(); };
    undoButton.onClick      = [this] { doUndo(); };
    redoButton.onClick      = [this] { doRedo(); };

    openButton.onClick   = [this] { openFile(); };
    saveButton.onClick   = [this] { saveFile(); };
    revertButton.onClick = [this] { revertAll(); };
    toolsButton.onClick  = [this] { showToolsMenu(); };

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
    else                                { processor.startRecording();
                                          if (onSourceNameChanged) onSourceNameChanged({}); }
    updateTransportButtonText();
}

void EditorToolbar::togglePlay()
{
    if (document.isRecording.load())    processor.stopRecording();
    else if (document.isPlaying.load()) processor.stopPlayback();
    else                               processor.startPlayback();
    updateTransportButtonText();
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
    enum { idStretch = 1, idChop, idExport };
    const bool empty = document.isEmpty();

    juce::PopupMenu m;
    m.addItem(idStretch, juce::String::fromUTF8("Stretch / Pitch\xE2\x80\xA6"), ! empty);
    m.addItem(idChop,    juce::String::fromUTF8("Chop to Grid\xE2\x80\xA6"),    ! empty);
    m.addItem(idExport,  juce::String::fromUTF8("Export Slices\xE2\x80\xA6"),   ! empty);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolsButton), [this](int r)
    {
        switch (r)
        {
            case idStretch: showStretchCallout(); break;
            case idChop:    showChopCallout();    break;
            case idExport:  exportSlices();       break;
            default: break;
        }
    });
}

void EditorToolbar::showAmplifyCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<AmplifyPanel>(document),
                                           amplifyButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showStretchCallout()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<StretchPanel>(document),
                                           toolsButton.getScreenBounds(), nullptr);
}

void EditorToolbar::showChopCallout()
{
    juce::CallOutBox::launchAsynchronously(
        std::make_unique<ChopPanel>(document, [this] { exportSlices(); }),
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

    const bool canUndo = document.undoManager.canUndo();
    undoButton.setEnabled(canUndo);
    redoButton.setEnabled(document.undoManager.canRedo());
    revertButton.setEnabled(canUndo);

    const bool sel = document.hasSelection();
    trimButton.setEnabled(sel);
    deleteButton.setEnabled(sel);
    cutButton.setEnabled(sel);
    copyButton.setEnabled(sel);
    pasteButton.setEnabled(clipboard.hasContent());
}

void EditorToolbar::timerCallback()
{
    updateTransportButtonText();

    double sr = document.getSampleRate() > 0 ? document.getSampleRate() : 44100.0;
    double posSec = (double) document.playhead.load() / sr;
    double lenSec = (double) document.getNumSamples() / sr;
    timeLabel.setText(formatTime(posSec) + " / " + formatTime(lenSec), juce::dontSendNotification);

    recLabel.setText(document.isRecording.load()
                         ? juce::String::fromUTF8("\xE2\x97\x8F REC  ") + mmss(posSec)
                         : juce::String(),
                     juce::dontSendNotification);
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
        if (file.existsAsFile() && document.loadFromFile(file, processor.getSampleRate()))
        {
            if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
            if (onSaved) onSaved();
        }
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
        if (file != juce::File() && document.saveToFile(file))
        {
            if (onSourceNameChanged) onSourceNameChanged(file.getFileName());
            if (onSaved) onSaved();
        }
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
    auto area = getLocalBounds();
    const int rowH = 26;
    const int gap = 4;

    // width < 0  -> flexible spacer; comp == nullptr with width >= 0 -> fixed spacer.
    auto flexRow = [&](juce::Rectangle<int> row,
                       std::initializer_list<std::pair<juce::Component*, int>> items)
    {
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        for (auto& [comp, w] : items)
        {
            if (comp == nullptr)
                fb.items.add(w < 0 ? juce::FlexItem().withFlex(1.0f)
                                   : juce::FlexItem().withWidth((float) w));
            else
                fb.items.add(juce::FlexItem(*comp).withWidth((float) w).withMinWidth(22.0f)
                                 .withMargin(juce::FlexItem::Margin(0, (float) gap, 0, 0)));
        }
        fb.performLayout(row);
    };

    flexRow(area.removeFromTop(rowH),
            { { &playButton, 58 }, { &loopButton, 56 }, { &timeLabel, 150 },
              { nullptr, -1 },
              { &recLabel, 118 }, { &recordButton, 80 } });

    area.removeFromTop(gap);
    flexRow(area.removeFromTop(rowH),
            { { &trimButton, 50 }, { &deleteButton, 58 }, { &silenceButton, 62 },
              { &normalizeButton, 76 }, { &amplifyButton, 74 }, { &reverseButton, 64 },
              { &fadeInButton, 62 }, { &fadeOutButton, 66 },
              { nullptr, 10 },
              { &cutButton, 46 }, { &copyButton, 50 }, { &pasteButton, 52 },
              { nullptr, 10 },
              { &undoButton, 34 }, { &redoButton, 34 },
              { nullptr, -1 } });

    area.removeFromTop(gap);
    flexRow(area.removeFromTop(rowH),
            { { &openButton, 62 }, { &saveButton, 76 }, { &revertButton, 62 },
              { nullptr, 10 }, { &toolsButton, 74 },
              { nullptr, -1 } });
}
