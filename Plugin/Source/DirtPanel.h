#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "DirtStage.h"
#include "DotMatrixLCD.h"

/**
    The Dirt popup (KnobRow's Dirt knob, "more" dot next to its caption): Drive / Rate / Bits for
    the pre-filter harmonics stage (DirtStage.h). Same dot-matrix LCD look as the effect popups.
    Drive is the same parameter as the knob-row Dirt knob; a timer keeps this panel in step with
    changes made there (and on state load) while it's pinned open.
*/
class DirtPanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit DirtPanel(AudioDocument& doc) : document(doc)
    {
        setLookAndFeel(&lnf);
        title.setText("DIRT", juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        addAndMakeVisible(title);

        setUpKnob(drive, "DRIVE", document.dirtDrive, 0.0,
                  [](double v) { return v < 0.005 ? juce::String("OFF") : juce::String(juce::roundToInt(v * 100.0)) + "%"; });
        setUpKnob(rate, "RATE", document.dirtRate, 1.0, [this](double v)
        {
            const double hz = r3wrk::DirtStage::rateHz(v, document.getSampleRate());
            if (hz <= 0.0) return juce::String("OFF");
            return hz >= 1000.0 ? juce::String(hz / 1000.0, 1) + "K" : juce::String(juce::roundToInt(hz));
        });
        setUpKnob(bits, "BITS", document.dirtBits, 1.0, [](double v)
        {
            const int b = r3wrk::DirtStage::bitDepth(v);
            return b == 0 ? juce::String("OFF") : juce::String(b) + " BIT";
        });

        setSize(236, 118);
        startTimerHz(15);
    }

    ~DirtPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        title.setBounds(r.removeFromTop(20));
        r.removeFromTop(4);
        const int w = r.getWidth() / 3;
        for (auto* k : { &drive, &rate, &bits })
        {
            auto col = r.removeFromLeft(w);
            k->caption.setBounds(col.removeFromTop(14));
            k->slider.setBounds(col);
        }
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
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, resetTo);   // Drive -> 0, Rate/Bits -> off
        k.slider.setValue(juce::jlimit(0.0, 1.0, target.load()), juce::dontSendNotification);
        k.slider.textFromValueFunction = std::move(textFn);
        k.slider.updateText();
        k.slider.onValueChange = [&k] { k.target->store(k.slider.getValue()); };
        addAndMakeVisible(k.slider);
    }

    void timerCallback() override   // follow changes made elsewhere (the row's Dirt knob, state load)
    {
        for (auto* k : { &drive, &rate, &bits })
        {
            if (k->slider.isMouseButtonDown())
                continue;
            const double v = juce::jlimit(0.0, 1.0, k->target->load());
            if (std::abs(v - k->slider.getValue()) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
    }

    AudioDocument& document;
    juce::Label title;
    Knob drive, rate, bits;
    lcd::HardwareLcdLookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DirtPanel)
};
