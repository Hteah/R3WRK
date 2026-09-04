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
}

EditorToolbar::EditorToolbar(EdisonCloneAudioProcessor& proc, AudioDocument& doc)
    : processor(proc), document(doc)
{
    for (auto* b : { &recordButton, &playButton, &openButton, &saveButton, &undoButton, &redoButton,
                      &waveViewButton, &specViewButton, &zoomInButton, &zoomOutButton, &zoomFitButton,
                      &cutButton, &copyButton, &pasteButton, &trimButton, &deleteButton, &normalizeButton,
                      &fadeInButton, &fadeOutButton, &reverseButton, &silenceButton, &applyGainButton,
                      &applyStretchButton, &recalcGridButton, &exportSlicesButton,
                      &setLoopStartButton, &setLoopEndButton })
        addAndMakeVisible(b);

    addAndMakeVisible(loopButton);
    addAndMakeVisible(gainSlider);
    addAndMakeVisible(stretchSlider);
    addAndMakeVisible(pitchSlider);
    addAndMakeVisible(bpmSlider);
    addAndMakeVisible(divisionBox);
    addAndMakeVisible(stretchLabel);
    addAndMakeVisible(pitchLabel);
    addAndMakeVisible(bpmLabel);
    addAndMakeVisible(statusLabel);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    statusLabel.setJustificationType(juce::Justification::centredRight);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    gainSlider.setRange(-24.0, 24.0, 0.1);
    gainSlider.setValue(0.0);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    stretchSlider.setRange(0.25, 4.0, 0.01);
    stretchSlider.setValue(1.0);
    stretchSlider.setSkewFactorFromMidPoint(1.0);
    stretchSlider.setTextValueSuffix("x");
    stretchSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    stretchSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    pitchSlider.setRange(-24.0, 24.0, 0.1);
    pitchSlider.setValue(0.0);
    pitchSlider.setTextValueSuffix(" st");
    pitchSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    bpmSlider.setRange(40.0, 240.0, 0.1);
    bpmSlider.setValue(120.0);
    bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    divisionBox.addItem("1/4", 4);
    divisionBox.addItem("1/8", 8);
    divisionBox.addItem("1/16", 16);
    divisionBox.addItem("1/32", 32);
    divisionBox.setSelectedId(16, juce::dontSendNotification);

    // --- wiring ---------------------------------------------------------
    recordButton.onClick = [this]
    {
        if (document.isRecording.load()) processor.stopRecording();
        else                              processor.startRecording();
        updateTransportButtonText();
    };
    playButton.onClick = [this]
    {
        if (document.isPlaying.load()) processor.stopPlayback();
        else                            processor.startPlayback();
        updateTransportButtonText();
    };
    loopButton.onClick = [this] { document.loopEnabled = loopButton.getToggleState(); };

    openButton.onClick = [this] { openFile(); };
    saveButton.onClick = [this] { saveFile(); };
    undoButton.onClick = [this] { document.undoManager.undo(); document.notifyChanged(); };
    redoButton.onClick = [this] { document.undoManager.redo(); document.notifyChanged(); };

    waveViewButton.onClick = [this] { if (onViewModeChanged) onViewModeChanged(false); };
    specViewButton.onClick = [this] { if (onViewModeChanged) onViewModeChanged(true); };
    zoomInButton.onClick   = [this] { if (onZoomIn)  onZoomIn(); };
    zoomOutButton.onClick  = [this] { if (onZoomOut) onZoomOut(); };
    zoomFitButton.onClick  = [this] { if (onZoomFit) onZoomFit(); };

    cutButton.onClick    = [this] { EditActions::cut(document, clipboard); };
    copyButton.onClick   = [this] { EditActions::copy(document, clipboard); };
    pasteButton.onClick  = [this] { EditActions::pasteReplace(document, clipboard); };
    trimButton.onClick   = [this] { EditActions::trimToSelection(document); };
    deleteButton.onClick = [this] { EditActions::deleteSelection(document); };
    normalizeButton.onClick = [this] { EditActions::normalize(document); };
    fadeInButton.onClick    = [this] { EditActions::fadeIn(document); };
    fadeOutButton.onClick   = [this] { EditActions::fadeOut(document); };
    reverseButton.onClick   = [this] { EditActions::reverse(document); };
    silenceButton.onClick   = [this] { EditActions::silence(document); };
    applyGainButton.onClick = [this] { EditActions::applyGainDb(document, (float) gainSlider.getValue()); };

    applyStretchButton.onClick = [this] { applyTimeStretch(); };

    recalcGridButton.onClick = [this]
    {
        document.chopBpm = bpmSlider.getValue();
        document.chopDivision = divisionBox.getSelectedId();
        document.recalculateChopMarkers();
        document.notifyChanged();
    };
    exportSlicesButton.onClick = [this] { exportSlices(); };

    setLoopStartButton.onClick = [this]
    {
        document.loopStart = document.hasSelection() ? document.getSelectionStart() : document.playhead.load();
        document.notifyChanged();
    };
    setLoopEndButton.onClick = [this]
    {
        document.loopEnd = document.hasSelection() ? document.getSelectionEnd() : document.playhead.load();
        document.notifyChanged();
    };

    document.changeBroadcaster.addChangeListener(this);
    updateTransportButtonText();
    startTimerHz(15);
}

EditorToolbar::~EditorToolbar()
{
    document.changeBroadcaster.removeChangeListener(this);
}

void EditorToolbar::updateTransportButtonText()
{
    recordButton.setButtonText(document.isRecording.load() ? "Stop Rec" : "Record");
    playButton.setButtonText(document.isPlaying.load() ? "Stop" : "Play");
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
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
    bpmSlider.setValue(document.chopBpm, juce::dontSendNotification);
    divisionBox.setSelectedId(document.chopDivision, juce::dontSendNotification);
    loopButton.setToggleState(document.loopEnabled.load(), juce::dontSendNotification);
}

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
    document.chopBpm = bpmSlider.getValue();
    document.chopDivision = divisionBox.getSelectedId();
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

void EditorToolbar::applyTimeStretch()
{
    auto range = document.getEffectiveRange();
    if (range.getLength() <= 0)
        return;

    auto& buf = document.getBuffer();
    juce::AudioBuffer<float> region(buf.getNumChannels(), (int) range.getLength());
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        region.copyFrom(ch, 0, buf, ch, (int) range.getStart(), (int) range.getLength());

    auto stretched = TimeStretchEngine::process(region, document.getSampleRate(),
                                                 stretchSlider.getValue(), pitchSlider.getValue());
    if (stretched.getNumSamples() > 0)
        EditActions::replaceRangeWith(document, range, stretched, "Time Stretch/Pitch");
}

void EditorToolbar::resized()
{
    auto area = getLocalBounds().reduced(4);
    const int rowH = 26;
    const int gap = 4;

    auto layoutRow = [&](juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Component*, int>> items,
                          juce::Component* flexTail = nullptr)
    {
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        for (auto& [comp, width] : items)
            fb.items.add(juce::FlexItem(*comp).withWidth((float) width).withMargin(juce::FlexItem::Margin(0, gap, 0, 0)));
        if (flexTail != nullptr)
            fb.items.add(juce::FlexItem(*flexTail).withFlex(1));
        fb.performLayout(row);
    };

    auto row1 = area.removeFromTop(rowH);
    layoutRow(row1, { { &recordButton, 70 }, { &playButton, 55 }, { &loopButton, 55 },
                       { &openButton, 65 }, { &saveButton, 75 }, { &undoButton, 55 }, { &redoButton, 55 },
                       { &waveViewButton, 80 }, { &specViewButton, 90 },
                       { &zoomInButton, 55 }, { &zoomOutButton, 55 }, { &zoomFitButton, 65 } },
              &statusLabel);

    area.removeFromTop(gap);
    auto row2 = area.removeFromTop(rowH);
    layoutRow(row2, { { &cutButton, 50 }, { &copyButton, 55 }, { &pasteButton, 55 }, { &trimButton, 50 },
                       { &deleteButton, 60 }, { &normalizeButton, 80 }, { &fadeInButton, 65 },
                       { &fadeOutButton, 70 }, { &reverseButton, 65 }, { &silenceButton, 65 },
                       { &gainSlider, 150 }, { &applyGainButton, 90 } });

    area.removeFromTop(gap);
    auto row3 = area.removeFromTop(rowH);
    layoutRow(row3, { { &stretchLabel, 60 }, { &stretchSlider, 120 }, { &pitchLabel, 55 }, { &pitchSlider, 120 },
                       { &applyStretchButton, 130 }, { &bpmLabel, 32 }, { &bpmSlider, 100 }, { &divisionBox, 75 },
                       { &recalcGridButton, 85 }, { &exportSlicesButton, 95 },
                       { &setLoopStartButton, 100 }, { &setLoopEndButton, 95 } });
}
