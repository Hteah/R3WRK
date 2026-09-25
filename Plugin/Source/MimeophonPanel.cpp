#include "MimeophonPanel.h"
#include "DotMatrixLCD.h"

namespace
{
    //==========================================================================
    // The full Mimeophon editor, launched in a CallOutBox from the "..." button (see
    // MimeophonPanel::openFullEditor()) -- same pattern as ReverbEditorPanel/PlexiphonEditorPanel.
    struct MimeophonEditorPanel : juce::Component
    {
        struct Knob { juce::Label caption; juce::Slider slider; };
        struct Toggle { juce::Label caption; juce::TextButton button; };

        explicit MimeophonEditorPanel(AudioDocument& doc) : document(doc)
        {
            // Hardware-LCD look (dot-matrix text/knobs) for this popup -- see DotMatrixLCD.h.
            setLookAndFeel(&lnf);

            title.setText("MIMEOPHON", juce::dontSendNotification);
            title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            addAndMakeVisible(title);

            setUpKnob(zone, "ZONE", document.mimeoZone, 0.0, 1.0,
                     [](double v) { return "ZONE " + juce::String(juce::jlimit(0, 7, juce::roundToInt(v * 7.0))); });
            setUpKnob(rate, "RATE", document.mimeoRate, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(repeats, "REPEATS", document.mimeoRepeats, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(skew, "SKEW", document.mimeoSkew, 0.0, 1.0,
                     [](double v) {
                         const int pct = juce::roundToInt((v - 0.5) * 200.0);
                         if (pct == 0) return juce::String("CTR");
                         return (pct > 0 ? "R" : "L") + juce::String(std::abs(pct)) + "%";
                     });
            setUpKnob(color, "COLOR", document.mimeoColor, 0.0, 1.0,
                     [](double v) {
                         const int pct = juce::roundToInt((v - 0.5) * 200.0);
                         return (pct >= 0 ? "+" : "") + juce::String(pct) + "%";
                     });
            setUpKnob(halo, "HALO", document.mimeoHalo, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });
            setUpKnob(mix, "MIX", document.mimeoMix, 0.0, 1.0,
                     [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; });

            // Crosses the Repeats feedback between channels so echoes alternate L->R->L->R --
            // the manual's "same button held" alternate mode for Skew, here a plain on/off
            // toggle (a plugin has no hold-to-engage gesture worth reproducing). Orthogonal to
            // the Skew knob above -- both can be on together.
            setUpToggle(pingPong, "PING-PONG", document.mimeoPingPong);

            setSize(320, 190);
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

        void setUpToggle(Toggle& t, const juce::String& caption, std::atomic<bool>& target)
        {
            t.caption.setText(caption, juce::dontSendNotification);
            t.caption.setJustificationType(juce::Justification::centred);
            t.caption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(t.caption);

            const bool on = target.load();
            t.button.setButtonText(on ? "ON" : "OFF");
            t.button.setClickingTogglesState(true);
            t.button.setToggleState(on, juce::dontSendNotification);
            t.button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            t.button.onClick = [this, &target, &t]
            {
                const bool nowOn = t.button.getToggleState();
                target.store(nowOn);
                t.button.setButtonText(nowOn ? "ON" : "OFF");
            };
            addAndMakeVisible(t.button);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(20));
            r.removeFromTop(6);

            const int gridCols = 4;   // fixed so both rows' columns line up, even with row 2's 3
            auto layoutRow = [&](juce::Rectangle<int> row, std::initializer_list<Knob*> knobs)
            {
                const int w = row.getWidth() / gridCols;
                for (auto* k : knobs)
                {
                    auto col = row.removeFromLeft(w);
                    k->caption.setBounds(col.removeFromTop(14));
                    k->slider.setBounds(col);
                }
                return row;   // whatever's left over (row 2's unused 4th column)
            };
            layoutRow(r.removeFromTop(70), { &zone, &rate, &repeats, &skew });

            auto row2Rest = layoutRow(r.removeFromTop(70), { &color, &halo, &mix });
            pingPong.caption.setBounds(row2Rest.removeFromTop(14));
            pingPong.button.setBounds(row2Rest.reduced(6, 10));
        }

        ~MimeophonEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

        AudioDocument& document;
        juce::Label title;
        Knob zone, rate, repeats, skew, color, halo, mix;
        Toggle pingPong;
        lcd::HardwareLcdLookAndFeel lnf;
    };
}

MimeophonPanel::MimeophonPanel(AudioDocument& doc) : document(doc)
{
    enablePill.onClick = [this]
    {
        const bool on = ! document.mimeoEnabled.load();
        document.mimeoEnabled.store(on);
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
    setUpKnob(rateKnob, "RATE", document.mimeoRate);
    setUpKnob(mixKnob,  "MIX",  document.mimeoMix);
    rateKnob.slider.setLookAndFeel(&knobLnF);
    mixKnob.slider.setLookAndFeel(&knobLnF);

    moreButton.setTooltip("Zone / Repeats / Skew / Color / Halo / Ping-Pong");
    moreButton.onClick = [this] { openFullEditor(); };
    moreButton.setLookAndFeel(&moreButtonLnf);   // boxless -- see the member's own comment
    addAndMakeVisible(moreButton);

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(6);
}

MimeophonPanel::~MimeophonPanel()
{
    rateKnob.slider.setLookAndFeel(nullptr);
    mixKnob.slider.setLookAndFeel(nullptr);
    moreButton.setLookAndFeel(nullptr);   // detach before moreButtonLnf is destroyed
    theme->removeChangeListener(this);
}

void MimeophonPanel::timerCallback()
{
    // Low-rate re-sync so an external change (a state/project load, undo) is reflected even
    // though this panel stays on screen continuously -- ReverbPanel/PlexiphonPanel/LfoPanel do
    // the same.
    const bool on = document.mimeoEnabled.load();
    if (on != enablePill.on) { enablePill.on = on; enablePill.repaint(); }

    auto resync = [](juce::Slider& s, double docVal)
    {
        if (! s.isMouseButtonDown() && std::abs(s.getValue() - docVal) > 1.0e-6)
            s.setValue(docVal, juce::dontSendNotification);
    };
    resync(rateKnob.slider, juce::jlimit(0.0, 1.0, document.mimeoRate.load()));
    resync(mixKnob.slider,  juce::jlimit(0.0, 1.0, document.mimeoMix.load()));
}

void MimeophonPanel::applyTheme()
{
    const auto& pal = theme->palette();
    enablePill.fill   = pal.accent;
    enablePill.ink    = pal.windowBg;
    enablePill.border = pal.textDim;
    enablePill.repaint();

    for (auto* k : { &rateKnob, &mixKnob })
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

void MimeophonPanel::openFullEditor()
{
    // Click: transient popup. Double-click: pinned until the button is clicked again.
    moreCallout.buttonClicked(moreButton, [this] { return std::make_unique<MimeophonEditorPanel>(document); });
}

void MimeophonPanel::EnablePill::paint(juce::Graphics& g)
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
    g.drawText("MIME", getLocalBounds(), juce::Justification::centred);
}

void MimeophonPanel::resized()
{
    // Packed from the left, sized to content -- same fix as ReverbPanel/PlexiphonPanel/LfoPanel.
    auto r = getLocalBounds().reduced(4, 2);
    constexpr int gap = 3, pillW = 32, pillH = 16, knobW = 53, moreW = 20, moreH = 16;   // pillW/pillH match KnobRow::ModelBadge's own size (the filter MNM/OT badge); knobW matches KnobRow's own upper bound (46-53 auto-fit)

    auto pillArea = r.removeFromLeft(pillW);
    enablePill.setBounds(pillArea.withSizeKeepingCentre(pillW, pillH));
    r.removeFromLeft(gap);

    for (auto* k : { &rateKnob, &mixKnob })
    {
        auto kcol = r.removeFromLeft(knobW);
        k->caption.setBounds(kcol.removeFromTop(17));   // matches KnobRow's own caption row height
        k->slider.setBounds(kcol);
        r.removeFromLeft(gap);
    }

    auto moreArea = r.removeFromLeft(moreW);
    moreButton.setBounds(moreArea.withSizeKeepingCentre(moreW, moreH));
}

void MimeophonPanel::paint(juce::Graphics&) {}
