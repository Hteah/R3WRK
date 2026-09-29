#pragma once
#include <JuceHeader.h>
#include "PinnableCallout.h"
#include "AudioDocument.h"
#include "MnmChorusEngine.h"
#include "DotMatrixLCD.h"
#include "SlotSwitchTab.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    The CHO cell of the FX drawer: the Monomachine-style chorus (MnmChorusEngine.h /
    PluginProcessor::applyChorus()). It shares the first slot with RTRG: the small tab above the
    pill switches the slot back to RTRG (FxRow shows whichever AudioDocument::fxSlotChorus picks,
    and only that one runs). The pill turns the chorus on/off; DEL and MIX sit in the drawer; the
    "more" dot opens all eight in hardware order (DEL DEP SPD MIX / FB WID LP INP). Readouts are
    the machine's raw 0..127, like the hardware. Same layout as the other drawer panels: pill, two
    knobs, dot.
*/
namespace choText
{
    inline juce::String raw(double v) { return juce::String(r3wrk::MnmChorusEngine::raw127(v)); }

    struct Spec { const char* name; std::atomic<double> AudioDocument::* field; int def; const char* tip; };
    // Hardware order and the machine's own defaults (double-click returns to them).
    inline const Spec kSpecs[8] = {
        { "DEL", &AudioDocument::chorusDel, 64,  "Delay time: 0.3 ms (0) to 22.7 ms (127)" },
        { "DEP", &AudioDocument::chorusDep, 64,  "Sweep depth of the three taps (scales with DEL)" },
        { "SPD", &AudioDocument::chorusSpd, 64,  "LFO speed: 0 = stopped, 64 = 0.5 Hz, 127 = 2 Hz" },
        { "MIX", &AudioDocument::chorusMix, 127, "Dry to wet. 127 = wet only (6 dB quieter); the middle gives the comb-y mix" },
        { "FB",  &AudioDocument::chorusFb,  0,   "Feedback of the taps (inverted); near 127 it rings almost forever" },
        { "WID", &AudioDocument::chorusWid, 0,   "Stereo width: offsets the right channel's LFO, up to 90 degrees" },
        { "LP",  &AudioDocument::chorusLp,  127, "Low-pass on the feedback only (dark repeats at low values)" },
        { "INP", &AudioDocument::chorusInp, 64,  "Input gain into a hard clip: above 64 hot signals distort" },
    };
}

class ChorusEditorPanel : public juce::Component,
                          private juce::Timer
{
public:
    explicit ChorusEditorPanel(AudioDocument& doc) : document(doc)
    {
        setLookAndFeel(&lnf);
        title.setText("CHORUS", juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        addAndMakeVisible(title);

        for (int i = 0; i < 8; ++i)
        {
            const auto& spec = choText::kSpecs[i];
            auto& k = knobs[i];
            k.target = &(document.*(spec.field));
            k.caption.setText(spec.name, juce::dontSendNotification);
            k.caption.setJustificationType(juce::Justification::centred);
            k.caption.setFont(juce::FontOptions(11.0f));
            addAndMakeVisible(k.caption);

            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
            k.slider.setRange(0.0, 1.0, 0.0);
            k.slider.setDoubleClickReturnValue(true, spec.def / 127.0);
            k.slider.setValue(juce::jlimit(0.0, 1.0, k.target->load()), juce::dontSendNotification);
            k.slider.textFromValueFunction = [](double v) { return choText::raw(v); };
            k.slider.updateText();
            k.slider.setTooltip(spec.tip);
            k.slider.onValueChange = [&k] { k.target->store(k.slider.getValue()); };
            addAndMakeVisible(k.slider);
        }

        setSize(300, 212);
        startTimerHz(15);
    }

    ~ChorusEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        title.setBounds(r.removeFromTop(20));
        r.removeFromTop(4);
        const int w = r.getWidth() / 4, rowH = r.getHeight() / 2;
        auto rows = std::array<juce::Rectangle<int>, 2> { r.removeFromTop(rowH), r };
        for (int i = 0; i < 8; ++i)
        {
            auto col = rows[(size_t) (i / 4)].removeFromLeft(w);
            knobs[i].caption.setBounds(col.removeFromTop(14));
            knobs[i].slider.setBounds(col);
        }
    }

private:
    struct Knob
    {
        juce::Label caption;
        juce::Slider slider;
        std::atomic<double>* target = nullptr;
    };

    void timerCallback() override
    {
        for (auto& k : knobs)
        {
            if (k.slider.isMouseButtonDown())
                continue;
            const double v = k.target->load();
            if (std::abs(v - k.slider.getValue()) > 1.0e-6)
                k.slider.setValue(v, juce::dontSendNotification);
        }
    }

    AudioDocument& document;
    juce::Label title;
    Knob knobs[8];
    lcd::HardwareLcdLookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChorusEditorPanel)
};

class ChorusPanel : public juce::Component,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    explicit ChorusPanel(AudioDocument& doc) : document(doc)
    {
        pill.onClick = [this]
        {
            document.chorusEnabled.store(! document.chorusEnabled.load());
            syncPill();
        };
        pill.setTooltip("CHO: click to switch the chorus on/off");
        addAndMakeVisible(pill);

        slotTab.target = "RTRG";
        slotTab.setTooltip("Switch this slot to RTRG (the chorus stops)");
        slotTab.onClick = [this] { document.fxSlotChorus.store(false); };
        addAndMakeVisible(slotTab);

        setUpKnob(delKnob, choText::kSpecs[0]);
        setUpKnob(mixKnob, choText::kSpecs[3]);

        moreButton.setTooltip("DEL DEP SPD MIX / FB WID LP INP (double-click to pin)");
        moreButton.onClick = [this]
        {
            moreCallout.buttonClicked(moreButton, [this] { return std::make_unique<ChorusEditorPanel>(document); });
        };
        moreButton.setLookAndFeel(&moreButtonLnf);
        addAndMakeVisible(moreButton);

        applyTheme();
        theme->addChangeListener(this);
        startTimerHz(15);
    }

    ~ChorusPanel() override
    {
        moreCallout.close();
        delKnob.slider.setLookAndFeel(nullptr);
        mixKnob.slider.setLookAndFeel(nullptr);
        moreButton.setLookAndFeel(nullptr);
        theme->removeChangeListener(this);
    }

    void resized() override
    {
        // Same packing as RetrigPanel (FxRow relies on it); the slot tab sits in the pill
        // column's caption row, level with the knob captions.
        auto r = getLocalBounds().reduced(4, 2);
        constexpr int gap = 3, pillW = 32, pillH = 16, knobW = 53, moreW = 20, moreH = 16, captionH = 17;
        auto pillCol = r.removeFromLeft(pillW);
        slotTab.setBounds(pillCol.withHeight(captionH).withSizeKeepingCentre(pillW, 13));
        pill.setBounds(pillCol.withSizeKeepingCentre(pillW, pillH));
        r.removeFromLeft(gap);
        for (auto* k : { &delKnob, &mixKnob })
        {
            auto kcol = r.removeFromLeft(knobW);
            k->caption.setBounds(kcol.removeFromTop(captionH));
            k->slider.setBounds(kcol);
            r.removeFromLeft(gap);
        }
        moreButton.setBounds(r.removeFromLeft(moreW).withSizeKeepingCentre(moreW, moreH));
    }

private:
    struct Pill : juce::Component, public juce::SettableTooltipClient
    {
        juce::Colour fill, ink, border;
        bool on = false, hovered = false;
        std::function<void()> onClick;
        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced(0.5f);
            constexpr float rad = 3.0f;
            g.setColour(on ? fill : fill.withAlpha(hovered ? 0.22f : 0.0f));
            g.fillRoundedRectangle(r, rad);
            g.setColour(border.withAlpha(on || hovered ? 0.95f : 0.55f));
            g.drawRoundedRectangle(r, rad, 1.0f);
            g.setColour(on ? ink : border);
            g.setFont(systemUIFont(10.0f));
            g.drawText("CHO", getLocalBounds(), juce::Justification::centred);
        }
        void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }
        void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hovered = false; repaint(); }
    };

    struct Knob
    {
        juce::Label caption;
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        std::atomic<double>* target = nullptr;
    };

    void setUpKnob(Knob& k, const choText::Spec& spec)
    {
        k.target = &(document.*(spec.field));
        k.caption.setText(spec.name, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(14.0f));
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, spec.def / 127.0);
        k.slider.setValue(juce::jlimit(0.0, 1.0, k.target->load()), juce::dontSendNotification);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
        k.slider.textFromValueFunction = [](double v) { return choText::raw(v); };
        k.slider.updateText();
        k.slider.setTooltip(spec.tip);
        k.slider.onValueChange = [&k] { k.target->store(k.slider.getValue()); };
        k.slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(k.caption);
        addAndMakeVisible(k.slider);
    }

    void syncPill()
    {
        const bool on = document.chorusEnabled.load();
        if (on != pill.on) { pill.on = on; pill.repaint(); }
    }

    void timerCallback() override
    {
        syncPill();
        for (auto* k : { &delKnob, &mixKnob })
        {
            const double v = k->target->load();
            if (! k->slider.isMouseButtonDown() && std::abs(k->slider.getValue() - v) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }

    void applyTheme()
    {
        const auto& pal = theme->palette();
        pill.fill = pal.accent;
        pill.ink = pal.windowBg;
        pill.border = pal.textDim;
        pill.repaint();
        slotTab.ink = pal.textDim;
        slotTab.border = pal.textDim;
        slotTab.repaint();
        for (auto* k : { &delKnob, &mixKnob })
        {
            k->caption.setColour(juce::Label::textColourId, pal.textDim);
            k->slider.setColour(juce::Slider::textBoxTextColourId, pal.text);
            k->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            k->caption.repaint();
            k->slider.repaint();
        }
        moreButton.setColour(juce::TextButton::textColourOffId, pal.textDim);
        moreButton.repaint();
    }

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    Pill pill;
    SlotSwitchTab slotTab;
    Knob delKnob, mixKnob;
    R3WRKLookAndFeel knobLnF;
    R3WRKIconOnlyLookAndFeel moreButtonLnf;
    juce::TextButton moreButton { R3WRKLookAndFeel::iconMore };
    PinnableCallout moreCallout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChorusPanel)
};
