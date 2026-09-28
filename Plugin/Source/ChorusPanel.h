#pragma once
#include <JuceHeader.h>
#include "PinnableCallout.h"
#include "AudioDocument.h"
#include "ChorusEngine.h"
#include "DotMatrixLCD.h"
#include "SlotSwitchTab.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    The CHO cell of the FX drawer: a BBD-style chorus (ChorusEngine.h /
    PluginProcessor::applyChorus()). It shares the first slot with RTRG: the small tab above the
    pill switches the slot back to RTRG (FxRow shows whichever AudioDocument::fxSlotChorus picks,
    and only that one runs). The pill turns the chorus on/off; MODE and METAL sit in the drawer;
    the "more" dot opens MODE / METAL / MIX / RATE / WIDTH / HISS / RING. Same layout as the other
    drawer panels: pill, two knobs, dot.
*/
namespace choText
{
    inline juce::String mode(double v) { return r3wrk::ChorusEngine::modeName(r3wrk::ChorusEngine::modeIndex(v)); }
    inline juce::String metal(double v)
    {
        const int pct = juce::roundToInt(v * 100.0);
        return pct == 0 ? juce::String("SMOOTH") : "METAL " + juce::String(pct) + "%";
    }
    inline juce::String percent(double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; }
    inline juce::String rate(double v, double mode01)
    {
        const double hz = r3wrk::ChorusEngine::rateHz(r3wrk::ChorusEngine::modeIndex(mode01), v);
        return juce::String(hz, hz < 10.0 ? 2 : 1) + " Hz";
    }
    inline juce::String hiss(double v) { return v <= 0.0005 ? juce::String("OFF") : percent(v); }
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

        setUpKnob(mode,  "MODE",  document.chorusMode,  0.0, [](double v) { return choText::mode(v); });
        setUpKnob(metal, "METAL", document.chorusMetal, 0.0, [](double v) { return choText::metal(v); });
        setUpKnob(mix,   "MIX",   document.chorusMix,   0.5, [](double v) { return choText::percent(v); });
        setUpKnob(rate,  "RATE",  document.chorusRate,  0.5,
                  [this](double v) { return choText::rate(v, document.chorusMode.load()); });
        setUpKnob(width, "WIDTH", document.chorusWidth, 1.0, [](double v) { return choText::percent(v); });
        setUpKnob(hiss,  "HISS",  document.chorusHiss,  0.0, [](double v) { return choText::hiss(v); });
        mode.slider.setTooltip("I / II: slow, wide sweeps. I+II: a fast, shallow shimmer");
        metal.slider.setTooltip("Shorter delay + feedback: from a smooth chorus to a ringing, metallic comb");
        mix.slider.setTooltip("50% = equal dry + wet (the classic sound). 100% = wet only (pitch vibrato)");

        ringCaption.setText("RING", juce::dontSendNotification);
        ringCaption.setJustificationType(juce::Justification::centred);
        ringCaption.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(ringCaption);
        ringButton.setClickingTogglesState(true);
        ringButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        ringButton.setTooltip("Feedback polarity with METAL up. +: bright, bell-like ring. -: hollow, robotic ring");
        ringButton.setToggleState(document.chorusRingNeg.load(), juce::dontSendNotification);
        ringButton.onClick = [this]
        {
            document.chorusRingNeg.store(ringButton.getToggleState());
            ringButtonText();
        };
        addAndMakeVisible(ringButton);
        ringButtonText();

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
        auto row1 = r.removeFromTop(rowH), row2 = r;
        for (auto* k : { &mode, &metal, &mix, &rate })
        {
            auto col = row1.removeFromLeft(w);
            k->caption.setBounds(col.removeFromTop(14));
            k->slider.setBounds(col);
        }
        for (auto* k : { &width, &hiss })
        {
            auto col = row2.removeFromLeft(w);
            k->caption.setBounds(col.removeFromTop(14));
            k->slider.setBounds(col);
        }
        auto col = row2.removeFromLeft(w);
        ringCaption.setBounds(col.removeFromTop(14));
        ringButton.setBounds(col.reduced(10, 16));
    }

private:
    struct Knob
    {
        juce::Label caption;
        juce::Slider slider;
        std::atomic<double>* target = nullptr;
    };

    void setUpKnob(Knob& k, const juce::String& caption, std::atomic<double>& target, double resetTo,
                   std::function<juce::String(double)> textFn)
    {
        k.target = &target;
        k.caption.setText(caption, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(k.caption);

        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, resetTo);
        k.slider.setValue(juce::jlimit(0.0, 1.0, target.load()), juce::dontSendNotification);
        k.slider.textFromValueFunction = std::move(textFn);
        k.slider.updateText();
        k.slider.onValueChange = [&k] { k.target->store(k.slider.getValue()); };
        addAndMakeVisible(k.slider);
    }

    void ringButtonText() { ringButton.setButtonText(ringButton.getToggleState() ? "-" : "+"); }

    void timerCallback() override
    {
        for (auto* k : { &mode, &metal, &mix, &rate, &width, &hiss })
        {
            if (k->slider.isMouseButtonDown())
                continue;
            const double v = k->target->load();
            if (std::abs(v - k->slider.getValue()) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
        const int m = r3wrk::ChorusEngine::modeIndex(document.chorusMode.load());
        if (m != shownMode) { shownMode = m; rate.slider.updateText(); }   // RATE reads in the mode's Hz
        const bool neg = document.chorusRingNeg.load();
        if (neg != ringButton.getToggleState())
        {
            ringButton.setToggleState(neg, juce::dontSendNotification);
            ringButtonText();
        }
    }

    AudioDocument& document;
    juce::Label title, ringCaption;
    Knob mode, metal, mix, rate, width, hiss;
    juce::TextButton ringButton;
    int shownMode = -1;
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

        setUpKnob(modeKnob, "MODE", document.chorusMode, 0.0, [](double v) { return choText::mode(v); });
        setUpKnob(metalKnob, "METAL", document.chorusMetal, 0.0, [](double v) { return choText::metal(v); });

        moreButton.setTooltip("Mode / Metal / Mix / Rate / Width / Hiss / Ring (double-click to pin)");
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
        modeKnob.slider.setLookAndFeel(nullptr);
        metalKnob.slider.setLookAndFeel(nullptr);
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
        for (auto* k : { &modeKnob, &metalKnob })
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

    void setUpKnob(Knob& k, const juce::String& caption, std::atomic<double>& target, double resetTo,
                   std::function<juce::String(double)> textFn)
    {
        k.target = &target;
        k.caption.setText(caption, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(14.0f));
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, resetTo);
        k.slider.setValue(juce::jlimit(0.0, 1.0, target.load()), juce::dontSendNotification);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
        k.slider.textFromValueFunction = std::move(textFn);
        k.slider.updateText();
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
        for (auto* k : { &modeKnob, &metalKnob })
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
        for (auto* k : { &modeKnob, &metalKnob })
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
    Knob modeKnob, metalKnob;
    R3WRKLookAndFeel knobLnF;
    R3WRKIconOnlyLookAndFeel moreButtonLnf;
    juce::TextButton moreButton { R3WRKLookAndFeel::iconMore };
    PinnableCallout moreCallout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChorusPanel)
};
