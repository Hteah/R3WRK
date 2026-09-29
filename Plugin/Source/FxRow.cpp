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
    // Panels reach further left than they draw (room for a nudged toggle -- see placeSlot), so
    // their empty area mustn't eat clicks meant for the slot underneath.
    for (juce::Component* p : { (juce::Component*) &retrigPanel, (juce::Component*) &chorusPanel, (juce::Component*) &mimeoPanel,
                                (juce::Component*) &plexPanel, (juce::Component*) &reverbPanel })
        p->setInterceptsMouseClicks(false, true);
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
    resetButton.setWantsKeyboardFocus(false);
    resetButton.setTooltip("Reset: stop, and put every knob and effect back to default");
    resetButton.setLookAndFeel(&resetLnF);
    resetButton.onClick = [this] { if (onReset) onReset(); };
    addAndMakeVisible(resetButton);

    startTimerHz(15);

    applyTheme();
    theme->addChangeListener(this);
}

FxRow::~FxRow()
{
    gainKnob.setLookAndFeel(nullptr);   // detach before gainLnF is destroyed
    resetButton.setLookAndFeel(nullptr);
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
    resetButton.setColour(juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
    resetButton.setColour(juce::TextButton::textColourOffId, pal.accent);   // the knobs' ring colour
    resetButton.setColour(juce::TextButton::textColourOnId,  pal.accent);
    repaint();
}

void FxRow::resized()
{
    // One row, three slots: RTRG-or-CHO | Delay (Mimeophon) | Reverb-or-Plexiphon, then Gain. (LFO is shelved -- see
    // class comment -- and takes no space here.) All three are real Components, each packed to
    // its own CONTENT width -- not stretched across an equal third of the row -- same "packed
    // from the left, sized to content" idiom each panel already uses for its own internal
    // pill+knobs.
    // Reset button: centred under the drawer toggle, vertically centred in the row. 42px box ->
    // the X's square is ~26px, the size of the open drawer toggle's glyph.
    constexpr int resetSide = 42;
    if (resetCentreX >= 0)
        resetButton.setBounds(juce::Rectangle<int>(resetSide, resetSide)
                                  .withCentre({ resetCentreX, getHeight() / 2 }));

    auto full = getLocalBounds().reduced(4, 2);
    constexpr int sectionGap = 16;   // matches KnobRow's dotGap between its own sections

    // Mimeophon/Plexiphon/Reverb's own resized() all pack pill(32) + gap(3) + 2*(knob 53 +
    // gap 3) + "..."(20) -- kept as a literal here too (not plumbed through as a shared
    // constant) so it can't silently drift from any one panel's own layout. knobW (53) matches
    // KnobRow's own auto-fit upper bound, and pillW (32) matches KnobRow::ModelBadge's own size,
    // so these read the same size as the top row's.
    constexpr int panelW = 60 + 3 + (53 + 3) * 2 + 20;   // 195 (toggle column 60: fits "[RTRG]" at 16pt bold)

    // The first slot starts so its toggle's left edge sits under the Pitch knob's disc (each
    // panel insets its toggle column by 4px -- see their resized()).
    if (firstEdge >= 0)
    {
        const int start = juce::jlimit(full.getX(), full.getRight(), firstEdge - 4);
        full.setLeft(start);
    }

    // Then the three slots are spread with equal gaps: up to Gain (which starts exactly under
    // End) in the Standalone, or out to End's right edge in the plugin (no Gain knob there).
    int gap = sectionGap, lastGap = sectionGap;
    if (! gainColumn.isEmpty())
    {
        if (showGain)
        {
            const int total = gainColumn.getStart() - full.getX() - 3 * panelW;
            gap = juce::jmax(4, total / 3);
            lastGap = juce::jmax(4, total - 2 * gap);   // takes the rounding remainder -> exact
        }
        else
        {
            const int total = gainColumn.getEnd() - full.getX() - 3 * panelW;
            gap = juce::jmax(4, total / 2);
        }
    }

    const auto modSlot = full.removeFromLeft(juce::jmin(panelW, full.getWidth()));   // RTRG or CHO
    placeSlot(retrigPanel, modSlot, "fx.mod");
    placeSlot(chorusPanel, modSlot, "fx.mod");
    full.removeFromLeft(gap);

    placeSlot(mimeoPanel, full.removeFromLeft(juce::jmin(panelW, full.getWidth())), "fx.dly");
    full.removeFromLeft(gap);

    const auto spaceSlot = full.removeFromLeft(juce::jmin(panelW, full.getWidth()));   // RVB or PLX
    placeSlot(plexPanel, spaceSlot, "fx.space");
    placeSlot(reverbPanel, spaceSlot, "fx.space");

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

    // The user's Edit Layout nudges (LayoutTweaks), on top of the automatic spacing above.
    auto nudge = [this](const juce::String& id, std::initializer_list<juce::Component*> comps)
    {
        if (const int dx = layout->get(id); dx != 0)
            for (auto* c : comps)
                c->setTopLeftPosition(c->getPosition().translated(dx, 0));
    };
    // (The three slots and their toggles were nudged in placeSlot.)
    if (showGain)
        nudge("fx.gain", { &gainCaption, &gainKnob });

    repaint();
}

template <typename Panel>
void FxRow::placeSlot(Panel& panel, juce::Rectangle<int> slot, const juce::String& id)
{
    // The panel's bounds reach kToggleSlack px further left than the slot (empty, and it lets
    // clicks through -- see the ctor), so its toggle can be nudged left without being clipped.
    panel.leftSlack = kToggleSlack;
    panel.setBounds(slot.withLeft(slot.getX() - kToggleSlack).translated(layout->get(id), 0));
    panel.resized();   // back to the automatic spot (a pure move doesn't re-run it) ...
    const auto parts = panel.getToggleParts();
    const int dx = juce::jmax(layout->get(id + ".toggle"), -parts.getFirst()->getX());
    if (dx != 0)       // ... then the toggle's own nudge
        for (auto* c : parts)
            c->setTopLeftPosition(c->getPosition().translated(dx, 0));
}

void FxRow::getLayoutItems(juce::Array<LayoutItem>& items)
{
    // A slot's guides are its visible children: the toggle, the knob discs (see layoutGuideRect).
    // Each slot is two items: the whole slot, and its toggle on its own (listed after, so it
    // wins the hit test where they overlap).
    juce::Array<LayoutItem> toggles;
    auto slot = [&](const juce::String& id, juce::Component& panel, int slack,
                    const juce::Array<juce::Component*>& parts, const juce::String& text)
    {
        LayoutItem it { id, this, panel.getBounds().withTrimmedLeft(slack), {} };
        LayoutItem tg { id + ".toggle", this, {}, {} };
        for (auto* c : panel.getChildren())
        {
            if (! c->isVisible() || dynamic_cast<juce::Label*>(c) != nullptr) continue;
            if (parts.contains(c))
                tg.bounds = tg.bounds.isEmpty() ? c->getBounds() + panel.getPosition()
                                                : tg.bounds.getUnion(c->getBounds() + panel.getPosition());
            else
                it.guides.add(layoutGuideRect(*c) + panel.getPosition());
        }
        tg.guides.add(toggleInkBounds(parts.getFirst()->getBounds() + panel.getPosition(), text));
        items.add(it);
        toggles.add(tg);
    };
    if (retrigPanel.isVisible()) slot("fx.mod", retrigPanel, retrigPanel.leftSlack, retrigPanel.getToggleParts(), retrigPanel.getToggleText());
    else                         slot("fx.mod", chorusPanel, chorusPanel.leftSlack, chorusPanel.getToggleParts(), chorusPanel.getToggleText());
    slot("fx.dly", mimeoPanel, mimeoPanel.leftSlack, mimeoPanel.getToggleParts(), mimeoPanel.getToggleText());
    if (plexPanel.isVisible()) slot("fx.space", plexPanel, plexPanel.leftSlack, plexPanel.getToggleParts(), plexPanel.getToggleText());
    else                       slot("fx.space", reverbPanel, reverbPanel.leftSlack, reverbPanel.getToggleParts(), reverbPanel.getToggleText());
    items.addArray(toggles);
    if (showGain && ! gainKnob.getBounds().isEmpty())
        items.add({ "fx.gain", this, gainCaption.getBounds().getUnion(gainKnob.getBounds()),
                    { layoutGuideRect(gainKnob) } });
}

void FxRow::paint(juce::Graphics& g)
{
    // Divider dots between the sections, like KnobRow's: three small dots centred in the gap
    // between one slot's "more" dot and the next slot's toggle text, and before Gain. Measured
    // from the same content rects the Edit Layout overlay uses, so they follow any nudges.
    juce::Array<LayoutItem> items;
    getLayoutItems(items);
    auto find = [&](const juce::String& id) -> const LayoutItem*
    {
        for (auto& it : items) if (it.id == id && ! it.guides.isEmpty()) return &it;
        return nullptr;
    };
    auto extent = [](const LayoutItem& it)
    {
        auto r = it.guides.getFirst();
        for (auto gr : it.guides) r = r.getUnion(gr);
        return r;
    };

    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;   // KnobRow::paint's dots
    g.setColour(theme->palette().text.withAlpha(0.4f));
    auto dotsBetween = [&](const char* leftId, const char* rightId)
    {
        const auto* l = find(leftId);
        const auto* r = find(rightId);
        if (l == nullptr || r == nullptr) return;
        const auto a = extent(*l), b = extent(*r);
        if (b.getX() <= a.getRight()) return;
        const float x = (float) (a.getRight() + b.getX()) * 0.5f;
        float y = (float) a.getCentreY() - (count - 1) * spacing * 0.5f;   // level with the slot's content
        for (int i = 0; i < count; ++i, y += spacing)
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    };
    dotsBetween("fx.mod", "fx.dly.toggle");
    dotsBetween("fx.dly", "fx.space.toggle");
    dotsBetween("fx.space", "fx.gain");
}
