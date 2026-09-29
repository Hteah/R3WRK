#include "FxRow.h"
#include <algorithm>

FxRow::FxRow(AudioDocument& doc, bool standalone)
    : lfoPanel(doc, standalone),
      retrigPanel(doc, standalone), chorusPanel(doc), mimeoPanel(doc), plexPanel(doc), reverbPanel(doc),
      document(doc), showGain(standalone)
{
    // lfoPanel is SHELVED (see class comment) -- constructed and fully wired (its
    // PluginProcessor/AudioDocument side is untouched), just not shown or laid out here.
    // Deliberately no addAndMakeVisible() -- bringing it back is exactly that one line plus its
    // old slot in resized()/paint() below.
    // Panels reach further left than they draw (room for a moved toggle -- see packSlot), so
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
    if (reverbPanel.isVisible() == reverb && plexPanel.isVisible() != reverb) return;
    reverbPanel.setVisible(reverb);
    plexPanel.setVisible(! reverb);
    resized();   // the two toggles' texts differ in width -> the rest re-packs
}

void FxRow::syncModSlot()
{
    const bool chorus = document.fxSlotChorus.load();
    if (chorusPanel.isVisible() == chorus && retrigPanel.isVisible() != chorus) return;
    chorusPanel.setVisible(chorus);
    retrigPanel.setVisible(! chorus);
    resized();
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

    // Packed left to right from the Pitch knob's disc edge, spaced from what's DRAWN (toggle text,
    // knob discs, the "more" ring -- not the components' boxes): 12px between a toggle and its
    // first knob and between the second knob and the "more" ring; 12px either side of the divider
    // dots (see paint()) between slots and before Gain. The knob pairs keep the panels' own spacing.
    int x = firstEdge >= 0 ? firstEdge : 8;

    const int afterRetrig = packSlot(retrigPanel, x, "fx.mod");   // RTRG or CHO
    const int afterChorus = packSlot(chorusPanel, x, "fx.mod");
    x = chorusPanel.isVisible() ? afterChorus : afterRetrig;

    x = packSlot(mimeoPanel, x, "fx.dly");

    const int afterPlex   = packSlot(plexPanel, x, "fx.space");   // RVB or PLX
    const int afterReverb = packSlot(reverbPanel, x, "fx.space");
    x = reverbPanel.isVisible() ? afterReverb : afterPlex;

    if (showGain)
    {
        // Gain's disc starts where the next slot's toggle would.
        const int w = gainColumn.isEmpty() ? 53 : gainColumn.getLength();   // End's width when known
        auto col = juce::Rectangle<int>(x, 2, w, getHeight() - 4);
        gainCaption.setBounds(col.removeFromTop(17));
        gainKnob.setBounds(col);
        const int dx = x - layoutGuideRect(gainKnob).getX();
        gainCaption.setTopLeftPosition(gainCaption.getPosition().translated(dx, 0));
        gainKnob.setTopLeftPosition(gainKnob.getPosition().translated(dx, 0));
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
    // (The three slots and their toggles were nudged in packSlot.)
    if (showGain)
        nudge("fx.gain", { &gainCaption, &gainKnob });

    repaint();
}

template <typename Panel>
int FxRow::packSlot(Panel& panel, int x, const juce::String& id)
{
    // The panel reaches kToggleSlack px further left than its own layout (empty, and it lets
    // clicks through -- see the ctor) and some spare width on the right, so its toggle and "more"
    // dot can be moved without being clipped.
    constexpr int panelW = 60 + 3 + (53 + 3) * 2 + 20;   // the panels' own pack: toggle 60, knobs 53, "more" 20
    panel.leftSlack = kToggleSlack;
    panel.setBounds(0, 2, kToggleSlack + panelW + 24, getHeight() - 4);
    panel.resized();   // the automatic spot (a pure move doesn't re-run it)

    juce::Array<juce::Slider*> knobs;
    juce::Component* more = nullptr;
    for (auto* c : panel.getChildren())
    {
        if (! c->isVisible()) continue;
        if (auto* sl = dynamic_cast<juce::Slider*>(c); sl != nullptr && sl->isRotary())
            knobs.add(sl);
        else if (auto* b = dynamic_cast<juce::TextButton*>(c);
                 b != nullptr && b->getButtonText() == R3WRKLookAndFeel::iconMore)
            more = b;
    }
    std::sort(knobs.begin(), knobs.end(), [](auto* a, auto* b) { return a->getX() < b->getX(); });

    const auto parts = panel.getToggleParts();
    auto shift = [](juce::Component* c, int dx) { c->setTopLeftPosition(c->getPosition().translated(dx, 0)); };
    auto ink = toggleInkBounds(parts.getFirst()->getBounds(), panel.getToggleText());   // panel coords
    int right = ink.getRight();

    if (knobs.size() >= 2 && more != nullptr)
    {
        const auto k1 = layoutGuideRect(*knobs.getFirst());
        const auto k2 = layoutGuideRect(*knobs[1]);
        const int dxT = (k1.getX() - kSpacing) - ink.getRight();         // toggle text -> 12 -> knob
        for (auto* c : parts) shift(c, dxT);
        ink.translate(dxT, 0);
        const auto ring = moreIconInkBounds(more->getBounds());
        shift(more, juce::roundToInt((float) (k2.getRight() + kSpacing) - ring.getX()));   // knob -> 12 -> ring
        right = juce::roundToInt(moreIconInkBounds(more->getBounds()).getRight());
    }

    // Toggle text starts at x; then the user's Edit Layout nudges on top.
    panel.setTopLeftPosition(x - ink.getX() + layout->get(id), 2);
    const int dxUser = juce::jmax(layout->get(id + ".toggle"), -parts.getFirst()->getX());
    if (dxUser != 0)
        for (auto* c : parts) shift(c, dxUser);

    return x + (right - ink.getX()) + kSpacing + kDotsW + kSpacing;   // -> dots -> next item
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
            else if (auto* b = dynamic_cast<juce::TextButton*>(c);
                     b != nullptr && b->getButtonText() == R3WRKLookAndFeel::iconMore)
                it.guides.add(moreIconInkBounds(c->getBounds() + panel.getPosition()).toNearestInt());
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
