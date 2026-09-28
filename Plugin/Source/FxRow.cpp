#include "FxRow.h"

FxRow::FxRow(AudioDocument& doc, bool standalone)
    : lfoPanel(doc, standalone),
      retrigPanel(doc, standalone), chorusPanel(doc), mimeoPanel(doc), plexPanel(doc), reverbPanel(doc),
      document(doc), showGain(standalone)
{
    // lfoPanel is SHELVED (see class comment) -- constructed and fully wired (its
    // PluginProcessor/AudioDocument side is untouched), just not shown or laid out here.
    // Deliberately no addAndMakeVisible() -- bringing it back is exactly that one line plus its
    // old slot in resized()/paint() below.
    addChildComponent(retrigPanel);   // syncModSlot() shows RTRG or CHO
    addChildComponent(chorusPanel);
    syncModSlot();
    addAndMakeVisible(mimeoPanel);
    addChildComponent(plexPanel);     // syncSpaceSlot() shows whichever model is selected
    addChildComponent(reverbPanel);
    syncSpaceSlot();

    if (showGain)
    {
        gainCaption.setText("GAIN", juce::dontSendNotification);
        gainCaption.setJustificationType(juce::Justification::centred);
        gainCaption.setFont(juce::FontOptions(14.0f));
        addAndMakeVisible(gainCaption);

        gainKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        gainKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
        gainKnob.setLookAndFeel(&gainLnF);
        gainKnob.setRange(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, 0.0);
        gainKnob.setDoubleClickReturnValue(true, 0.0);   // 0 dB = default volume
        gainKnob.textFromValueFunction = [](double db)
        {
            if (db <= AudioDocument::kMinGainDb + 0.05)
                return juce::String::fromUTF8("-\xE2\x88\x9E dB");   // "-inf dB" (mute)
            return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
        };
        gainKnob.setValue(document.playbackGainDb.load(), juce::dontSendNotification);
        gainKnob.updateText();
        gainKnob.onValueChange = [this] { document.playbackGainDb.store(gainKnob.getValue()); };
        addAndMakeVisible(gainKnob);
    }
    startTimerHz(15);

    applyTheme();
    theme->addChangeListener(this);
}

FxRow::~FxRow()
{
    gainKnob.setLookAndFeel(nullptr);   // detach before gainLnF is destroyed
    theme->removeChangeListener(this);
}

void FxRow::syncSpaceSlot()
{
    const bool reverb = document.reverbEnabled.load();
    reverbPanel.setVisible(reverb);
    plexPanel.setVisible(! reverb);
}

void FxRow::syncModSlot()
{
    const bool chorus = document.fxSlotChorus.load();
    chorusPanel.setVisible(chorus);
    retrigPanel.setVisible(! chorus);
}

void FxRow::timerCallback()
{
    syncSpaceSlot();
    syncModSlot();
    if (! showGain || gainKnob.isMouseButtonDown())
        return;
    const double db = document.playbackGainDb.load();
    if (std::abs(db - gainKnob.getValue()) > 1.0e-6)
        gainKnob.setValue(db, juce::dontSendNotification);
}

void FxRow::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
}

void FxRow::applyTheme()
{
    // mimeoPanel/plexPanel/reverbPanel each theme themselves (SharedResourcePointer
    // <ThemeManager>, same ChangeListener pattern) -- the only thing left to refresh here is
    // the Gain knob, coloured the same way KnobRow colours its own knobs.
    const auto& pal = theme->palette();
    gainCaption.setColour(juce::Label::textColourId, pal.textDim);
    gainKnob.setColour(juce::Slider::rotarySliderFillColourId, pal.accent);
    gainKnob.setColour(juce::Slider::textBoxTextColourId, pal.text);
    gainKnob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    repaint();
}

void FxRow::resized()
{
    // One row, three slots: RTRG-or-CHO | Delay (Mimeophon) | Reverb-or-Plexiphon, then Gain. (LFO is shelved -- see
    // class comment -- and takes no space here.) All three are real Components, each packed to
    // its own CONTENT width -- not stretched across an equal third of the row -- same "packed
    // from the left, sized to content" idiom each panel already uses for its own internal
    // pill+knobs.
    auto full = getLocalBounds().reduced(4, 2);
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections

    // Mimeophon/Plexiphon/Reverb's own resized() all pack pill(32) + gap(3) + 2*(knob 53 +
    // gap 3) + "..."(20) -- kept as a literal here too (not plumbed through as a shared
    // constant) so it can't silently drift from any one panel's own layout. knobW (53) matches
    // KnobRow's own auto-fit upper bound, and pillW (32) matches KnobRow::ModelBadge's own size,
    // so these read the same size as the top row's.
    constexpr int panelW = 32 + 3 + (53 + 3) * 2 + 20;   // 167

    // With Gain showing and End's position known, spread the three gaps evenly so Gain's column
    // starts exactly where End's does (clamped so the effects never touch or drift apart).
    int gap = sectionGap, lastGap = sectionGap;
    if (showGain && ! gainColumn.isEmpty())
    {
        const int total = gainColumn.getStart() - full.getX() - 3 * panelW;
        gap = juce::jlimit(4, 40, total / 3);
        lastGap = juce::jlimit(4, 40, total - 2 * gap);   // takes the rounding remainder -> exact
    }

    const auto modSlot = full.removeFromLeft(juce::jmin(panelW, full.getWidth()));   // RTRG or CHO
    retrigPanel.setBounds(modSlot);
    chorusPanel.setBounds(modSlot);
    full.removeFromLeft(gap);

    mimeoPanel.setBounds(full.removeFromLeft(juce::jmin(panelW, full.getWidth())));
    full.removeFromLeft(gap);

    const auto spaceSlot = full.removeFromLeft(juce::jmin(panelW, full.getWidth()));   // RVB or PLX
    plexPanel.setBounds(spaceSlot);
    reverbPanel.setBounds(spaceSlot);

    if (showGain && full.getWidth() > lastGap + 40)
    {
        full.removeFromLeft(lastGap);
        const int w = gainColumn.isEmpty() ? 53 : gainColumn.getLength();   // End's width when known
        auto col = full.removeFromLeft(juce::jmin(w, full.getWidth()));
        gainCaption.setBounds(col.removeFromTop(17));
        gainKnob.setBounds(col);
    }
    // Whatever's left on the right stays empty, same as KnobRow leaving the drawer toggle's own
    // margin -- not stretched into it.

    repaint();
}

void FxRow::paint(juce::Graphics&)
{
    // Nothing to draw: the effect slots are separated by spacing alone (no divider dots).
}
