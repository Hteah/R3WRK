#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "DotMatrixLCD.h"

/**
    The Overdub popup (the "more" dot beside the toolbar's Overdub button): LEVEL of the new layer,
    FEEDBACK (how much of what's already there survives each pass) and MONITOR (hear your input
    over the loop). Same dot-matrix LCD look as the effect popups; a timer keeps it in step with
    state loads while it's pinned open.
*/
class OverdubPanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit OverdubPanel(AudioDocument& doc) : document(doc)
    {
        setLookAndFeel(&lnf);
        title.setText("OVERDUB", juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        addAndMakeVisible(title);

        setUpKnob(level, "LEVEL", document.overdubLevel, 0.0, 2.0, 1.0, [](double v)
        {
            if (v < 0.001) return juce::String("-INF");
            const double db = juce::Decibels::gainToDecibels(v);
            return (db > 0.05 ? "+" : "") + juce::String(db, 1) + " DB";
        });
        level.slider.setSkewFactorFromMidPoint(0.5);   // finer control around unity and below
        setUpKnob(feedback, "FEEDBACK", document.overdubFeedback, 0.0, 1.0, 1.0, [](double v)
        {
            return juce::String(juce::roundToInt(v * 100.0)) + "%";
        });

        monitorCaption.setText("MONITOR", juce::dontSendNotification);
        monitorCaption.setJustificationType(juce::Justification::centred);
        monitorCaption.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(monitorCaption);
        monitorButton.setClickingTogglesState(true);
        monitorButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        monitorButton.setTooltip("Hear your input over the loop while overdubbing (use headphones)");
        monitorButton.onClick = [this]
        {
            document.overdubMonitor.store(monitorButton.getToggleState());
            syncMonitorText();
        };
        addAndMakeVisible(monitorButton);
        monitorButton.setToggleState(document.overdubMonitor.load(), juce::dontSendNotification);
        syncMonitorText();

        setSize(236, 118);
        startTimerHz(15);
    }

    ~OverdubPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        title.setBounds(r.removeFromTop(20));
        r.removeFromTop(4);
        const int w = r.getWidth() / 3;
        for (auto* k : { &level, &feedback })
        {
            auto col = r.removeFromLeft(w);
            k->caption.setBounds(col.removeFromTop(14));
            k->slider.setBounds(col);
        }
        monitorCaption.setBounds(r.removeFromTop(14));
        monitorButton.setBounds(r.reduced(6, 12));
    }

private:
    struct Knob
    {
        juce::Label caption;
        juce::Slider slider;
        std::atomic<double>* target = nullptr;
    };

    void setUpKnob(Knob& k, const juce::String& caption, std::atomic<double>& target,
                   double lo, double hi, double resetTo, std::function<juce::String(double)> textFn)
    {
        k.target = &target;
        k.caption.setText(caption, juce::dontSendNotification);
        k.caption.setJustificationType(juce::Justification::centred);
        k.caption.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(k.caption);

        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        k.slider.setRange(lo, hi, 0.0);
        k.slider.setDoubleClickReturnValue(true, resetTo);
        k.slider.setValue(juce::jlimit(lo, hi, target.load()), juce::dontSendNotification);
        k.slider.textFromValueFunction = std::move(textFn);
        k.slider.updateText();
        k.slider.onValueChange = [&k] { k.target->store(k.slider.getValue()); };
        addAndMakeVisible(k.slider);
    }

    void syncMonitorText() { monitorButton.setButtonText(monitorButton.getToggleState() ? "ON" : "OFF"); }

    void timerCallback() override
    {
        for (auto* k : { &level, &feedback })
        {
            if (k->slider.isMouseButtonDown())
                continue;
            const double v = k->target->load();
            if (std::abs(v - k->slider.getValue()) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
        const bool mon = document.overdubMonitor.load();
        if (mon != monitorButton.getToggleState())
        {
            monitorButton.setToggleState(mon, juce::dontSendNotification);
            syncMonitorText();
        }
    }

    AudioDocument& document;
    juce::Label title, monitorCaption;
    Knob level, feedback;
    juce::TextButton monitorButton;
    lcd::HardwareLcdLookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OverdubPanel)
};
