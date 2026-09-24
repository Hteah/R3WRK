#include "ReverbPanel.h"
#include "DotMatrixLCD.h"

namespace
{
    //==========================================================================
    // The full reverb editor, launched in a CallOutBox from the "..." button (see
    // ReverbPanel::openFullEditor()) -- same pattern as LfoPanel's per-instance editor
    // (LfoEditorPanel) and EditorToolbar's other popups (AutoRecordThresholdPanel, ...).
    struct ReverbEditorPanel : juce::Component
    {
        struct Knob { juce::Label caption; juce::Slider slider; };

        explicit ReverbEditorPanel(AudioDocument& doc) : document(doc)
        {
            // Hardware-LCD look (dot-matrix text/knobs) for this popup -- see DotMatrixLCD.h.
            // Applied to the panel itself so every child (title, captions, sliders) inherits it.
            setLookAndFeel(&lnf);

            title.setText("REVERB", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            addAndMakeVisible(title);

            setUpKnob(size, "SIZE", document.reverbSize, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(absorb, "ABSORB", document.reverbAbsorb, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(decay, "DECAY", document.reverbDecay, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(width, "WIDTH", document.reverbWidth, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 200.0)) + "%"; });
            setUpKnob(tilt, "TILT", document.reverbTilt, 0.0, 1.0,
                     [](double v) {
                         const int pct = juce::roundToInt((v - 0.5) * 200.0);
                         return (pct >= 0 ? "+" : "") + juce::String(pct) + "%";
                     });
            setUpKnob(mix, "MIX", document.reverbMix, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(predelay, "PRE-DELAY", document.reverbPredelay, 0.0, 1.0,
                     [](double v) { return juce::String(juce::jmap(v, 7.0, 500.0), 0) + " ms"; });

            setSize(330, 190);
        }

        void setUpKnob(Knob& k, const juce::String& caption, std::atomic<double>& target,
                       double lo, double hi, std::function<juce::String(double)> textFn)
        {
            k.caption.setText(caption, juce::dontSendNotification);
            k.caption.setJustificationType(juce::Justification::centred);
            k.caption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(k.caption);

            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
            k.slider.setRange(lo, hi, 0.0);
            k.slider.setValue(juce::jlimit(lo, hi, target.load()), juce::dontSendNotification);
            k.slider.textFromValueFunction = std::move(textFn);
            k.slider.updateText();
            k.slider.onValueChange = [this, &target, &k] { target.store(k.slider.getValue()); };
            addAndMakeVisible(k.slider);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(20));
            r.removeFromTop(6);

            const int gridCols = 4;   // fixed so both rows' columns line up, even with row 2's 3
            auto layoutRow = [&](std::initializer_list<Knob*> knobs)
            {
                auto row = r.removeFromTop(70);
                const int w = row.getWidth() / gridCols;
                for (auto* k : knobs)
                {
                    auto col = row.removeFromLeft(w);
                    k->caption.setBounds(col.removeFromTop(14));
                    k->slider.setBounds(col);
                }
            };
            layoutRow({ &size, &absorb, &decay, &width });
            layoutRow({ &tilt, &mix, &predelay });
        }

        ~ReverbEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

        AudioDocument& document;
        juce::Label title;
        Knob size, absorb, decay, width, tilt, mix, predelay;
        lcd::HardwareLcdLookAndFeel lnf;
    };
}

ReverbPanel::ReverbPanel(AudioDocument& doc) : document(doc)
{
    enablePill.onClick = [this]
    {
        const bool on = ! document.reverbEnabled.load();
        document.reverbEnabled.store(on);
        enablePill.on = on;
        enablePill.repaint();
    };
    addAndMakeVisible(enablePill);

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
    setUpKnob(decayKnob, "DECAY", document.reverbDecay);
    setUpKnob(mixKnob,   "MIX",   document.reverbMix);
    decayKnob.slider.setLookAndFeel(&knobLnF);
    mixKnob.slider.setLookAndFeel(&knobLnF);

    moreButton.setTooltip("Size / Absorb / Tilt / Pre-delay");
    moreButton.onClick = [this] { openFullEditor(); };
    moreButton.setLookAndFeel(&moreButtonLnf);   // boxless -- see the member's own comment
    addAndMakeVisible(moreButton);

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(6);
}

ReverbPanel::~ReverbPanel()
{
    decayKnob.slider.setLookAndFeel(nullptr);
    mixKnob.slider.setLookAndFeel(nullptr);
    moreButton.setLookAndFeel(nullptr);   // detach before moreButtonLnf is destroyed
    theme->removeChangeListener(this);
}

void ReverbPanel::timerCallback()
{
    // Low-rate re-sync so an external change (a state/project load, undo) is reflected even
    // though this panel stays on screen continuously -- LfoPanel's timerCallback does the same.
    // Skip a slider the user's actively dragging, so this never fights their gesture.
    const bool on = document.reverbEnabled.load();
    if (on != enablePill.on) { enablePill.on = on; enablePill.repaint(); }

    auto resync = [](juce::Slider& s, double docVal)
    {
        if (! s.isMouseButtonDown() && std::abs(s.getValue() - docVal) > 1.0e-6)
            s.setValue(docVal, juce::dontSendNotification);
    };
    resync(decayKnob.slider, juce::jlimit(0.0, 1.0, document.reverbDecay.load()));
    resync(mixKnob.slider,   juce::jlimit(0.0, 1.0, document.reverbMix.load()));
}

void ReverbPanel::applyTheme()
{
    const auto& pal = theme->palette();
    enablePill.fill   = pal.accent;
    enablePill.ink    = pal.windowBg;
    enablePill.border = pal.textDim;
    enablePill.repaint();

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

void ReverbPanel::openFullEditor()
{
    juce::CallOutBox::launchAsynchronously(std::make_unique<ReverbEditorPanel>(document),
                                           moreButton.getScreenBounds(), nullptr);
}

void ReverbPanel::EnablePill::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    constexpr float rad = 3.0f;
    if (on)
        g.setColour(fill);
    else
        g.setColour(fill.withAlpha(hovered ? 0.22f : 0.0f));
    g.fillRoundedRectangle(r, rad);
    g.setColour(border.withAlpha(on || hovered ? 0.95f : 0.55f));
    g.drawRoundedRectangle(r, rad, 1.0f);
    g.setColour(on ? ink : border);
    // systemUIFont(), not Space Mono -- matches KnobRow::ModelBadge's own fix (see its comment):
    // a proportional UI font stays legible at this pill's small size where the monospace app
    // default reads cramped.
    g.setFont(systemUIFont(11.0f));
    g.drawText("RVRB", getLocalBounds(), juce::Justification::centred);
}

void ReverbPanel::resized()
{
    // Packed from the left, sized to content -- pill, two knobs, "..." right next to Mix --
    // rather than stretched to fill the whole cell (same fix as LfoPanel's rows/Add button:
    // this panel gets the same half-row width DELAY's placeholder does, far more than a pill +
    // two knobs + a button actually need, so any leftover space collects on the right instead
    // of being spread out between the controls).
    auto r = getLocalBounds().reduced(4, 2);
    constexpr int gap = 3, pillW = 32, pillH = 16, knobW = 53, moreW = 20, moreH = 16;   // pillW/pillH match KnobRow::ModelBadge's own size (the filter MNM/OT badge); knobW matches KnobRow's own upper bound (46-53 auto-fit)

    auto pillArea = r.removeFromLeft(pillW);
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

void ReverbPanel::paint(juce::Graphics&) {}
