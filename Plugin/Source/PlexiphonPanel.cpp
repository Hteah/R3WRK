#include "PlexiphonPanel.h"
#include "DotMatrixLCD.h"

namespace
{
    //==========================================================================
    // The full Plexiphon editor, launched in a CallOutBox from the "..." button (see
    // PlexiphonPanel::openFullEditor()) -- same pattern as ReverbEditorPanel/LfoEditorPanel.
    struct PlexiphonEditorPanel : juce::Component
    {
        struct Knob { juce::Label caption; juce::Slider slider; };

        explicit PlexiphonEditorPanel(AudioDocument& doc) : document(doc)
        {
            // Hardware-LCD look (dot-matrix text/knobs) for this popup -- see DotMatrixLCD.h.
            setLookAndFeel(&lnf);

            title.setText("PLEXIPHON", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            addAndMakeVisible(title);

            setUpKnob(level, "LEVEL", document.plexLevel, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(plexus, "PLEXUS", document.plexPlexus, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(size, "SIZE", document.plexSize, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(diffuse, "DIFFUSE", document.plexDiffuse, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(decay, "DECAY", document.plexDecay, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(color, "COLOR", document.plexColor, 0.0, 1.0,
                     [](double v) {
                         const int pct = juce::roundToInt((v - 0.5) * 200.0);
                         return (pct >= 0 ? "+" : "") + juce::String(pct) + "%";
                     });
            setUpKnob(mix, "MIX", document.plexMix, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            // v2 stereo: COUPLE = L/R cross-feed inside the feedback; SKEW = L/R pushed apart.
            setUpKnob(couple, "COUPLE", document.plexCouple, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(skew, "SKEW", document.plexSkew, 0.0, 1.0,
                     [](double v) {
                         const int pct = juce::roundToInt((v - 0.5) * 200.0);
                         if (pct == 0) return juce::String("CTR");
                         return (pct > 0 ? "L" : "R") + juce::String(std::abs(pct)) + "%";
                     });

            setSize(400, 190);
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

            const int gridCols = 5;   // fixed so both rows' columns line up, even with row 2's 4
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
            layoutRow({ &level, &plexus, &size, &diffuse, &decay });
            layoutRow({ &color, &couple, &skew, &mix });
        }

        ~PlexiphonEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

        AudioDocument& document;
        juce::Label title;
        Knob level, plexus, size, diffuse, decay, color, mix, couple, skew;
        lcd::HardwareLcdLookAndFeel lnf;
    };
}

PlexiphonPanel::PlexiphonPanel(AudioDocument& doc) : document(doc)
{
    // RVB and PLX share one drawer slot. The pill is the slot's on/off (AudioDocument::spaceOn --
    // off lets the tail ring out); the small tab above it switches the slot to RVB, like the
    // RTRG/CHO slot's.
    enablePill.onClick = [this] { document.spaceOn.store(! document.spaceOn.load()); };
    enablePill.setTooltip("PLX: click to switch on/off (off lets the tail ring out)");
    addAndMakeVisible(enablePill);

    slotTab.target = "RVB";
    slotTab.setTooltip("Switch this slot to RVB");
    slotTab.onClick = [this]
    {
        document.reverbEnabled.store(true);
        document.plexEnabled.store(false);
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
    setUpKnob(plexusKnob, "PLEXUS", document.plexPlexus);
    setUpKnob(mixKnob,    "MIX",    document.plexMix);
    plexusKnob.slider.setLookAndFeel(&knobLnF);
    mixKnob.slider.setLookAndFeel(&knobLnF);

    moreButton.setTooltip("Level / Size / Diffuse / Decay / Color");
    moreButton.onClick = [this] { openFullEditor(); };
    moreButton.setLookAndFeel(&moreButtonLnf);   // boxless -- see the member's own comment
    addAndMakeVisible(moreButton);

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(6);
}

PlexiphonPanel::~PlexiphonPanel()
{
    plexusKnob.slider.setLookAndFeel(nullptr);
    mixKnob.slider.setLookAndFeel(nullptr);
    moreButton.setLookAndFeel(nullptr);   // detach before moreButtonLnf is destroyed
    theme->removeChangeListener(this);
}

void PlexiphonPanel::timerCallback()
{
    // Low-rate re-sync so an external change (a state/project load, undo) is reflected even
    // though this panel stays on screen continuously -- ReverbPanel/LfoPanel do the same.
    const bool on = document.spaceOn.load();   // lit = the slot is on
    if (on != enablePill.on) { enablePill.on = on; enablePill.repaint(); }

    auto resync = [](juce::Slider& s, double docVal)
    {
        if (! s.isMouseButtonDown() && std::abs(s.getValue() - docVal) > 1.0e-6)
            s.setValue(docVal, juce::dontSendNotification);
    };
    resync(plexusKnob.slider, juce::jlimit(0.0, 1.0, document.plexPlexus.load()));
    resync(mixKnob.slider,    juce::jlimit(0.0, 1.0, document.plexMix.load()));
}

void PlexiphonPanel::applyTheme()
{
    const auto& pal = theme->palette();
    enablePill.fill   = pal.text;                                             // on ink
    enablePill.ink    = pal.windowBg;   // (unused by the bracket toggle)
    enablePill.border = pal.textDim;
    enablePill.repaint();
    slotTab.ink    = pal.textDim;
    slotTab.border = pal.textDim;
    slotTab.repaint();

    for (auto* k : { &plexusKnob, &mixKnob })
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

void PlexiphonPanel::openFullEditor()
{
    // Click: transient popup. Double-click: pinned until the button is clicked again.
    moreCallout.buttonClicked(moreButton, [this] { return std::make_unique<PlexiphonEditorPanel>(document); });
}

void PlexiphonPanel::EnablePill::paint(juce::Graphics& g)
{
    // fill = on ink, border = off ink (see applyTheme()).
    drawBracketToggle(g, getLocalBounds().toFloat(), "PLX", on, hovered, fill, border);
}

void PlexiphonPanel::resized()
{
    // Packed from the left, sized to content -- same fix as ReverbPanel/LfoPanel's own layout.
    auto r = getLocalBounds().reduced(4, 2);
    constexpr int gap = 3, pillW = 60, pillH = 22, knobW = 53, moreW = 20, moreH = 16;   // pillW/pillH match KnobRow::ModelBadge's own size (the filter MNM/OT badge); knobW matches KnobRow's own upper bound (46-53 auto-fit)

    auto pillArea = r.removeFromLeft(pillW);
    slotTab.setBounds(pillArea.withHeight(17).withSizeKeepingCentre(pillW, 13));   // caption row, like RetrigPanel's
    enablePill.setBounds(pillArea.withSizeKeepingCentre(pillW, pillH));
    r.removeFromLeft(gap);

    for (auto* k : { &plexusKnob, &mixKnob })
    {
        auto kcol = r.removeFromLeft(knobW);
        k->caption.setBounds(kcol.removeFromTop(17));   // matches KnobRow's own caption row height
        k->slider.setBounds(kcol);
        r.removeFromLeft(gap);
    }

    auto moreArea = r.removeFromLeft(moreW);
    moreButton.setBounds(moreArea.withSizeKeepingCentre(moreW, moreH));
}

void PlexiphonPanel::paint(juce::Graphics&) {}
