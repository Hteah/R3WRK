#include "ShimmerPanel.h"
#include "ShimmerEngine.h"
#include "DotMatrixLCD.h"

namespace
{
    //==========================================================================
    // The full SHM editor (the "more" dot) -- same CallOutBox + hardware-LCD pattern as
    // ShimmerEditorPanel / MimeophonEditorPanel.
    struct ShimmerEditorPanel : juce::Component
    {
        struct Knob { juce::Label caption; juce::Slider slider; };
        struct Toggle { juce::Label caption; juce::TextButton button; };

        explicit ShimmerEditorPanel(AudioDocument& doc) : document(doc)
        {
            setLookAndFeel(&lnf);

            title.setText("SHIMMER", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            addAndMakeVisible(title);

            auto pct = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
            setUpKnob(size, "SIZE", document.shimmerSize, pct);
            setUpKnob(decay, "DECAY", document.shimmerDecay, [](double v)
            {
                const double s = r3wrk::ShimmerReverb::decaySeconds(v);
                return juce::String(s, s < 10.0 ? 1 : 0) + " s";
            });
            setUpKnob(tone, "TONE", document.shimmerTone, [](double v)
            {
                const int p = juce::roundToInt((v - 0.5) * 200.0);
                if (p == 0) return juce::String("CTR");
                return (p < 0 ? "LP " : "HP ") + juce::String(std::abs(p)) + "%";
            });
            setUpKnob(width, "WIDTH", document.shimmerWidth, pct);
            setUpKnob(mix, "MIX", document.shimmerMix, pct);
            setUpKnob(shimmer, "SHIMMER", document.shimmerAmount, pct);
            setUpKnob(movement, "MOVEMENT", document.shimmerMovement, pct);
            setUpToggle(interval, "INTERVAL", document.shimmerFifth, "5TH", "OCT");
            setUpToggle(freeze, "FREEZE", document.shimmerFreeze, "ON", "OFF");

            setSize(400, 190);
        }

        void setUpKnob(Knob& k, const juce::String& caption, std::atomic<double>& target,
                       std::function<juce::String(double)> textFn)
        {
            k.caption.setText(caption, juce::dontSendNotification);
            k.caption.setJustificationType(juce::Justification::centred);
            k.caption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(k.caption);

            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
            k.slider.setRange(0.0, 1.0, 0.0);
            k.slider.setValue(juce::jlimit(0.0, 1.0, target.load()), juce::dontSendNotification);
            k.slider.textFromValueFunction = std::move(textFn);
            k.slider.updateText();
            k.slider.onValueChange = [this, &target, &k] { target.store(k.slider.getValue()); };
            addAndMakeVisible(k.slider);
        }

        void setUpToggle(Toggle& t, const juce::String& caption, std::atomic<bool>& target,
                         const juce::String& onText, const juce::String& offText)
        {
            t.caption.setText(caption, juce::dontSendNotification);
            t.caption.setJustificationType(juce::Justification::centred);
            t.caption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(t.caption);

            const bool on = target.load();
            t.button.setButtonText(on ? onText : offText);
            t.button.setClickingTogglesState(true);
            t.button.setToggleState(on, juce::dontSendNotification);
            t.button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            t.button.onClick = [this, &target, &t, onText, offText]
            {
                const bool nowOn = t.button.getToggleState();
                target.store(nowOn);
                t.button.setButtonText(nowOn ? onText : offText);
            };
            addAndMakeVisible(t.button);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(20));
            r.removeFromTop(6);

            constexpr int gridCols = 5;
            const int w = r.getWidth() / gridCols;
            auto knobAt = [](Knob& k, juce::Rectangle<int> col)
            {
                k.caption.setBounds(col.removeFromTop(14));
                k.slider.setBounds(col);
            };
            auto toggleAt = [](Toggle& t, juce::Rectangle<int> col)
            {
                t.caption.setBounds(col.removeFromTop(14));
                t.button.setBounds(col.reduced(6, 10));
            };
            auto row1 = r.removeFromTop(70);
            for (auto* k : { &size, &decay, &tone, &width, &mix })
                knobAt(*k, row1.removeFromLeft(w));
            auto row2 = r.removeFromTop(70);
            knobAt(shimmer, row2.removeFromLeft(w));
            toggleAt(interval, row2.removeFromLeft(w));
            knobAt(movement, row2.removeFromLeft(w));
            row2.removeFromLeft(w);
            toggleAt(freeze, row2.removeFromLeft(w));
        }

        ~ShimmerEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

        AudioDocument& document;
        juce::Label title;
        Knob size, decay, tone, width, mix, shimmer, movement;
        Toggle interval, freeze;
        lcd::HardwareLcdLookAndFeel lnf;
    };
}

ShimmerPanel::ShimmerPanel(AudioDocument& doc) : document(doc)
{
    // RVB and SHM share one drawer slot. The pill is the slot's on/off (AudioDocument::spaceOn --
    // off lets the tail ring out); the small tab above it switches the slot to RVB.
    enablePill.onClick = [this] { document.spaceOn.store(! document.spaceOn.load()); };
    enablePill.setTooltip("SHM: click to switch on/off (off lets the tail ring out)");
    addAndMakeVisible(enablePill);

    slotTab.target = "RVB";
    slotTab.setTooltip("Switch this slot to RVB");
    slotTab.onClick = [this]
    {
        document.reverbEnabled.store(true);
        document.shimmerEnabled.store(false);
    };
    addAndMakeVisible(slotTab);

    auto setUpKnob = [this](Knob& k, const juce::String& caption, std::atomic<double>& target)
    {
        k.caption.setText(caption, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(14.0f));   // matches KnobRow's own caption size
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setValue(juce::jlimit(0.0, 1.0, target.load()), juce::dontSendNotification);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.updateText();
        k.slider.onValueChange = [this, &target, &k] { target.store(k.slider.getValue()); };
        addAndMakeVisible(k.caption);
        addAndMakeVisible(k.slider);
    };
    setUpKnob(decayKnob, "DECAY", document.shimmerDecay);
    setUpKnob(mixKnob,   "MIX",   document.shimmerMix);
    decayKnob.slider.textFromValueFunction = [](double v)
    {
        const double s = r3wrk::ShimmerReverb::decaySeconds(v);
        return juce::String(s, s < 10.0 ? 1 : 0) + " s";
    };
    decayKnob.slider.updateText();
    decayKnob.slider.setLookAndFeel(&knobLnF);
    mixKnob.slider.setLookAndFeel(&knobLnF);

    moreButton.setTooltip("Size / Tone / Width / Shimmer / Interval / Movement / Freeze");
    moreButton.onClick = [this] { openFullEditor(); };
    moreButton.setLookAndFeel(&moreButtonLnf);   // boxless -- see the member's own comment
    addAndMakeVisible(moreButton);

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(6);
}

ShimmerPanel::~ShimmerPanel()
{
    decayKnob.slider.setLookAndFeel(nullptr);
    mixKnob.slider.setLookAndFeel(nullptr);
    moreButton.setLookAndFeel(nullptr);   // detach before moreButtonLnf is destroyed
    theme->removeChangeListener(this);
}

void ShimmerPanel::timerCallback()
{
    // Low-rate re-sync so an external change (a state/project load, undo) is reflected even
    // though this panel stays on screen continuously -- LfoPanel's timerCallback does the same.
    // Skip a slider the user's actively dragging, so this never fights their gesture.
    const bool on = document.spaceOn.load();   // lit = the slot is on
    if (on != enablePill.on) { enablePill.on = on; enablePill.repaint(); }

    auto resync = [](juce::Slider& s, double docVal)
    {
        if (! s.isMouseButtonDown() && std::abs(s.getValue() - docVal) > 1.0e-6)
            s.setValue(docVal, juce::dontSendNotification);
    };
    resync(decayKnob.slider, juce::jlimit(0.0, 1.0, document.shimmerDecay.load()));
    resync(mixKnob.slider,   juce::jlimit(0.0, 1.0, document.shimmerMix.load()));
}

void ShimmerPanel::applyTheme()
{
    const auto& pal = theme->palette();
    enablePill.fill   = pal.text;                                             // on ink
    enablePill.ink    = pal.windowBg;   // (unused by the bracket toggle)
    enablePill.border = pal.textDim;
    enablePill.repaint();
    slotTab.ink    = pal.textDim;
    slotTab.border = pal.textDim;
    slotTab.repaint();

    for (auto* k : { &decayKnob, &mixKnob })
    {
        k->caption.setColour(juce::Label::textColourId, pal.textDim);
        // Matches KnobRow's own knobs: the default LookAndFeel_V4 textbox outline was never
        // cleared here, so these readouts had a visible box the top knob row's don't.
        k->slider.setColour(juce::Slider::textBoxTextColourId, pal.text);
        k->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k->caption.repaint();
        k->slider.repaint();
    }

    moreButton.setColour(juce::TextButton::textColourOffId, pal.textDim);
    moreButton.repaint();
}

void ShimmerPanel::openFullEditor()
{
    // Click: transient popup. Double-click: pinned until the button is clicked again.
    moreCallout.buttonClicked(moreButton, [this] { return std::make_unique<ShimmerEditorPanel>(document); });
}

void ShimmerPanel::EnablePill::paint(juce::Graphics& g)
{
    // fill = on ink, border = off ink (see applyTheme()).
    drawBracketToggle(g, getLocalBounds().toFloat(), "SHM", on, hovered, fill, border);
}

void ShimmerPanel::resized()
{
    // Packed from the left, sized to content -- pill, two knobs, "..." right next to Mix --
    // rather than stretched to fill the whole cell (same fix as LfoPanel's rows/Add button:
    // this panel gets the same half-row width DELAY's placeholder does, far more than a pill +
    // two knobs + a button actually need, so any leftover space collects on the right instead
    // of being spread out between the controls).
    auto r = getLocalBounds().withTrimmedLeft(leftSlack).reduced(4, 2);
    constexpr int gap = 3, pillW = 60, pillH = 22, knobW = 53, moreW = 20, moreH = 16;   // pillW/pillH match KnobRow::ModelBadge's own size (the filter MNM/OT badge); knobW matches KnobRow's own upper bound (46-53 auto-fit)

    auto pillArea = r.removeFromLeft(pillW);
    slotTab.setBounds(pillArea.withHeight(17).withSizeKeepingCentre(pillW, 13));   // caption row, like RetrigPanel's
    enablePill.setBounds(pillArea.withSizeKeepingCentre(pillW, pillH));
    r.removeFromLeft(gap);

    for (auto* k : { &decayKnob, &mixKnob })
    {
        auto kcol = r.removeFromLeft(knobW);
        k->caption.setBounds(kcol.removeFromTop(17));   // matches KnobRow's own caption row height
        k->slider.setBounds(kcol);
        r.removeFromLeft(gap);
    }

    auto moreArea = r.removeFromLeft(moreW);
    moreButton.setBounds(moreArea.withSizeKeepingCentre(moreW, moreH));
}

void ShimmerPanel::paint(juce::Graphics&) {}
