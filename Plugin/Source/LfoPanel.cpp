#include "LfoPanel.h"

namespace
{
    juce::String shapeName(r3wrk::LfoShape s)
    {
        switch (s)
        {
            case r3wrk::LfoShape::sine:       return "SIN";
            case r3wrk::LfoShape::square:     return "SQR";
            case r3wrk::LfoShape::triangle:   return "TRI";
            case r3wrk::LfoShape::saw:        return "SAW";
            case r3wrk::LfoShape::sampleHold: default: return "S&H";
        }
    }

    juce::String rangeName(r3wrk::LfoRateRange r)
    {
        switch (r)
        {
            case r3wrk::LfoRateRange::slow:   return "SLOW";
            case r3wrk::LfoRateRange::audio:  return "AUD";
            case r3wrk::LfoRateRange::normal: default: return "NORM";
        }
    }

    juce::String targetName(r3wrk::ModTarget t)
    {
        switch (t)
        {
            case r3wrk::ModTarget::filterBase:  return "Filter Base";
            case r3wrk::ModTarget::filterWidth: return "Filter Width";
            case r3wrk::ModTarget::filterHpQ:   return "Filter HP Q";
            case r3wrk::ModTarget::filterLpQ:   return "Filter LP Q";
            case r3wrk::ModTarget::gain:        return "Output Gain";
            case r3wrk::ModTarget::none:        default: return "None";
        }
    }

    juce::String rateReadout(double rate01, r3wrk::LfoRateRange range)
    {
        const double hz = r3wrk::lfoRateHz(rate01, range);
        if (hz < 1.0)
        {
            const double secs = 1.0 / hz;
            return secs >= 60.0 ? juce::String(secs / 60.0, 1) + " min"
                                 : juce::String(secs, 1) + " s";
        }
        return juce::String(hz, hz < 10.0 ? 2 : 1) + " Hz";
    }

    //==========================================================================
    // The full per-LFO editor, launched in a CallOutBox from a row click (see
    // LfoPanel::openEditorFor()) -- same pattern as EditorToolbar's own popups
    // (AutoRecordThresholdPanel, BlackBoxDurationPanel, ...).
    struct LfoEditorPanel : juce::Component
    {
        LfoEditorPanel(AudioDocument& doc, int slot, bool standalone) : document(doc), slotIndex(slot)
        {
            auto& s = document.lfoSlots[slotIndex];

            title.setText("LFO " + juce::String(slotIndex + 1), juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            addAndMakeVisible(title);

            enableButton.setClickingTogglesState(true);
            enableButton.setToggleState(s.enabled.load(), juce::dontSendNotification);
            enableButton.onClick = [this] { document.lfoSlots[slotIndex].enabled.store(enableButton.getToggleState()); };
            addAndMakeVisible(enableButton);

            shapeCaption.setText("Shape", juce::dontSendNotification);
            shapeCaption.setFont(juce::FontOptions(11.0f));
            shapeCaption.setColour(juce::Label::textColourId, juce::Colours::grey);
            addAndMakeVisible(shapeCaption);
            {
                constexpr int groupId = 8801;
                const r3wrk::LfoShape shapes[] = { r3wrk::LfoShape::sine, r3wrk::LfoShape::square,
                                                   r3wrk::LfoShape::triangle, r3wrk::LfoShape::saw,
                                                   r3wrk::LfoShape::sampleHold };
                const int current = s.shape.load();
                for (auto sh : shapes)
                {
                    auto* b = shapeButtons.add(new juce::TextButton(shapeName(sh)));
                    b->setRadioGroupId(groupId);
                    b->setClickingTogglesState(true);
                    b->setToggleState((int) sh == current, juce::dontSendNotification);
                    b->onClick = [this, sh] { document.lfoSlots[slotIndex].shape.store((int) sh); };
                    addAndMakeVisible(b);
                }
            }

            rangeCaption.setText("Range", juce::dontSendNotification);
            rangeCaption.setFont(juce::FontOptions(11.0f));
            rangeCaption.setColour(juce::Label::textColourId, juce::Colours::grey);
            addAndMakeVisible(rangeCaption);
            {
                constexpr int groupId = 8802;
                const r3wrk::LfoRateRange ranges[] = { r3wrk::LfoRateRange::slow, r3wrk::LfoRateRange::normal,
                                                       r3wrk::LfoRateRange::audio };
                const int current = s.rateRange.load();
                for (auto rg : ranges)
                {
                    auto* b = rangeButtons.add(new juce::TextButton(rangeName(rg)));
                    b->setRadioGroupId(groupId);
                    b->setClickingTogglesState(true);
                    b->setToggleState((int) rg == current, juce::dontSendNotification);
                    b->onClick = [this, rg] { document.lfoSlots[slotIndex].rateRange.store((int) rg); rate.updateText(); };
                    addAndMakeVisible(b);
                }
            }

            rate.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            rate.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
            rate.setRange(0.0, 1.0, 0.0);
            rate.setValue(s.rate01.load(), juce::dontSendNotification);
            rate.textFromValueFunction = [this](double v)
            {
                return rateReadout(v, (r3wrk::LfoRateRange) document.lfoSlots[slotIndex].rateRange.load());
            };
            rate.updateText();
            rate.onValueChange = [this] { document.lfoSlots[slotIndex].rate01.store(rate.getValue()); };
            rateCaption.setText("RATE", juce::dontSendNotification);
            rateCaption.setJustificationType(juce::Justification::centred);
            rateCaption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(rate);
            addAndMakeVisible(rateCaption);

            // Not wired to host BPM yet -- see the class comment on LfoPanel.h.
            syncButton.setEnabled(false);
            syncButton.setTooltip("Tempo sync to the host -- coming soon");
            addAndMakeVisible(syncButton);

            targetCaption.setText("Target", juce::dontSendNotification);
            targetCaption.setFont(juce::FontOptions(11.0f));
            targetCaption.setColour(juce::Label::textColourId, juce::Colours::grey);
            addAndMakeVisible(targetCaption);
            targetButton.onClick = [this, standalone] { showTargetMenu(standalone); };
            addAndMakeVisible(targetButton);
            refreshTargetButtonText();

            amount.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            amount.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
            amount.setRange(-1.0, 1.0, 0.0);
            amount.setDoubleClickReturnValue(true, 0.0);
            amount.setValue(s.amount.load(), juce::dontSendNotification);
            amount.textFromValueFunction = [](double v)
            {
                return (v > 0.0 ? "+" : "") + juce::String(juce::roundToInt(v * 100.0)) + "%";
            };
            amount.updateText();
            amount.onValueChange = [this] { document.lfoSlots[slotIndex].amount.store(amount.getValue()); };
            amountCaption.setText("AMOUNT", juce::dontSendNotification);
            amountCaption.setJustificationType(juce::Justification::centred);
            amountCaption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(amount);
            addAndMakeVisible(amountCaption);

            setSize(300, 256);
        }

        void showTargetMenu(bool standalone)
        {
            // Filter targets route through a TPT state-variable filter (r3wrk::
            // ModulatedMultiModeFilter, BiquadFilter.h), not the ordinary direct-form biquad --
            // see PluginProcessor::applyModulatedFilter()'s comment. That filter is verified
            // offline (Tests/SmokeTest.cpp) to stay stable under continuous fast modulation,
            // which the direct-form one measurably isn't (it produced non-finite output under
            // the same stress).
            juce::PopupMenu m;
            const r3wrk::ModTarget targets[] = { r3wrk::ModTarget::none, r3wrk::ModTarget::filterBase,
                                                 r3wrk::ModTarget::filterWidth, r3wrk::ModTarget::filterHpQ,
                                                 r3wrk::ModTarget::filterLpQ };
            for (auto t : targets)
                m.addItem(targetName(t), [this, t] { setTarget(t); });
            if (standalone)
                m.addItem(targetName(r3wrk::ModTarget::gain), [this] { setTarget(r3wrk::ModTarget::gain); });
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(targetButton));
        }

        void setTarget(r3wrk::ModTarget t)
        {
            document.lfoSlots[slotIndex].target.store((int) t);
            refreshTargetButtonText();
        }

        void refreshTargetButtonText()
        {
            targetButton.setButtonText(targetName((r3wrk::ModTarget) document.lfoSlots[slotIndex].target.load()));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            auto top = r.removeFromTop(20);
            enableButton.setBounds(top.removeFromRight(80));
            title.setBounds(top);
            r.removeFromTop(8);

            shapeCaption.setBounds(r.removeFromTop(14));
            {
                auto row = r.removeFromTop(24);
                const int w = row.getWidth() / juce::jmax(1, shapeButtons.size());
                for (auto* b : shapeButtons) b->setBounds(row.removeFromLeft(w).reduced(1));
            }
            r.removeFromTop(8);

            rangeCaption.setBounds(r.removeFromTop(14));
            {
                auto row = r.removeFromTop(24);
                const int w = row.getWidth() / juce::jmax(1, rangeButtons.size());
                for (auto* b : rangeButtons) b->setBounds(row.removeFromLeft(w).reduced(1));
            }
            r.removeFromTop(8);

            auto knobs = r.removeFromTop(64);
            auto rateCol = knobs.removeFromLeft(knobs.getWidth() / 2);
            rateCaption.setBounds(rateCol.removeFromTop(14));
            rate.setBounds(rateCol);
            amountCaption.setBounds(knobs.removeFromTop(14));
            amount.setBounds(knobs);
            r.removeFromTop(8);

            targetCaption.setBounds(r.removeFromTop(14));
            auto targetRow = r.removeFromTop(24);
            syncButton.setBounds(targetRow.removeFromRight(56));
            targetRow.removeFromRight(4);
            targetButton.setBounds(targetRow);
        }

        AudioDocument& document;
        int slotIndex;
        juce::Label title, shapeCaption, rangeCaption, targetCaption, rateCaption, amountCaption;
        juce::TextButton enableButton { "ENABLED" };
        juce::OwnedArray<juce::TextButton> shapeButtons, rangeButtons;
        juce::Slider rate, amount;
        juce::TextButton targetButton, syncButton { "SYNC" };
    };
}

LfoPanel::LfoPanel(AudioDocument& doc, bool standalone)
    : document(doc), standaloneBuild(standalone)
{
    addButton.setTooltip("Add another LFO");
    addButton.onClick = [this]
    {
        const int n = document.numVisibleLfos.load();
        if (n < AudioDocument::kMaxLfos)
        {
            document.numVisibleLfos.store(n + 1);
            rebuildRows();
        }
    };
    addAndMakeVisible(addButton);

    rebuildRows();
    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(6);
}

LfoPanel::~LfoPanel()
{
    theme->removeChangeListener(this);
}

void LfoPanel::rebuildRows()
{
    const int n = juce::jlimit(0, AudioDocument::kMaxLfos, document.numVisibleLfos.load());
    if (n == lastBuiltCount)
        return;

    rows.clear();
    for (int i = 0; i < n; ++i)
        addAndMakeVisible(rows.add(new Row(*this, i)));

    lastBuiltCount = n;
    applyTheme();
    resized();
}

void LfoPanel::timerCallback()
{
    rebuildRows();   // picks up numVisibleLfos changing from a state load while this is on screen
    for (auto* r : rows)
        r->repaint();   // enable/target can also change from the popup editor
}

void LfoPanel::applyTheme()
{
    const auto& pal = theme->palette();
    for (auto* r : rows)
    {
        r->fill    = pal.accent;
        r->ink     = pal.windowBg;
        r->border  = pal.textDim;
        r->textDim = pal.textDim;
        r->repaint();
    }
    addButton.setColour(juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
    addButton.setColour(juce::TextButton::textColourOffId, pal.textDim);
}

void LfoPanel::openEditorFor(int slotIndex, juce::Component& anchor)
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<LfoEditorPanel>(document, slotIndex, standaloneBuild),
                                           anchor.getScreenBounds(), nullptr);
}

void LfoPanel::resized()
{
    auto r = getLocalBounds().reduced(2);
    const bool canAdd = document.numVisibleLfos.load() < AudioDocument::kMaxLfos;
    addButton.setVisible(canAdd);

    juce::Rectangle<int> addArea;
    if (canAdd)
        addArea = r.removeFromBottom(18);

    const int n = juce::jmax(1, rows.size());
    const int rowH = juce::jlimit(14, 20, r.getHeight() / n);
    for (auto* row : rows)
        row->setBounds(r.removeFromTop(rowH));

    if (canAdd)
        addButton.setBounds(addArea);
}

void LfoPanel::paint(juce::Graphics&) {}

void LfoPanel::Row::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto pillArea = r.removeFromLeft((float) kPillW).reduced(2.0f, 4.0f);

    auto& slot = owner.document.lfoSlots[index];
    const bool on = slot.enabled.load(std::memory_order_relaxed);

    g.setColour(on ? fill : fill.withAlpha(hovered ? 0.18f : 0.0f));
    g.fillRoundedRectangle(pillArea, 3.0f);
    g.setColour(border.withAlpha(on || hovered ? 0.9f : 0.5f));
    g.drawRoundedRectangle(pillArea, 3.0f, 1.0f);

    juce::String label = "LFO " + juce::String(index + 1);
    const auto target = (r3wrk::ModTarget) slot.target.load(std::memory_order_relaxed);
    if (on && target != r3wrk::ModTarget::none)
        label << "  " << juce::String::fromUTF8("\xe2\x86\x92") << "  " << targetName(target);   // "->"

    g.setColour(textDim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(label, r.reduced(4.0f, 0.0f), juce::Justification::centredLeft);

    if (hovered)
    {
        g.setColour(textDim.withAlpha(0.35f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 1.0f);
    }
}

void LfoPanel::Row::mouseUp(const juce::MouseEvent& e)
{
    if (e.position.x < (float) kPillW)
    {
        auto& slot = owner.document.lfoSlots[index];
        slot.enabled.store(! slot.enabled.load(std::memory_order_relaxed));
        repaint();
    }
    else
    {
        owner.openEditorFor(index, *this);
    }
}
