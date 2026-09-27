#pragma once
#include <JuceHeader.h>
#include "PinnableCallout.h"
#include "AudioDocument.h"
#include "RetrigEngine.h"
#include "DotMatrixLCD.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    The RTRG cell of the FX drawer (first slot, left of DLY): Octatrack-style buffer retrig
    (RetrigEngine.h / PluginProcessor::applyRetrig()). The pill latches the stutter on/off; TIME
    and FADE sit in the drawer; the "more" dot opens TIME / FADE / SYNC-FREE / BPM (the BPM only
    matters in the Standalone -- VST/AU follow the host's tempo). Same layout as the other drawer
    panels: pill, two knobs, dot.
*/
namespace rtrgText
{
    inline juce::String time(double v, bool synced)
    {
        if (synced)
            return r3wrk::RetrigEngine::kNotes[r3wrk::RetrigEngine::noteIndex(v)].name;
        return juce::String(juce::roundToInt(r3wrk::RetrigEngine::freeMs(v))) + " ms";
    }
    inline juce::String fade(double v)
    {
        const int pct = juce::roundToInt(std::abs(v - 0.5) * 200.0);
        if (pct == 0) return "PLAIN";
        return (v < 0.5 ? "OUT " : "UP ") + juce::String(pct) + "%";
    }
}

class RetrigEditorPanel : public juce::Component,
                          private juce::Timer
{
public:
    RetrigEditorPanel(AudioDocument& doc, bool standaloneIn) : document(doc), standalone(standaloneIn)
    {
        setLookAndFeel(&lnf);
        title.setText("RETRIG", juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        addAndMakeVisible(title);

        setUpKnob(time, "TIME", document.rtrgTime, 0.0, 1.0, 0.55,
                  [this](double v) { return rtrgText::time(v, document.rtrgSync.load()); });
        setUpKnob(fade, "FADE", document.rtrgFade, 0.0, 1.0, 0.5, [](double v) { return rtrgText::fade(v); });
        setUpKnob(bpm, "BPM", document.rtrgBpm, 20.0, 300.0, 120.0, [this](double v)
        {
            const double host = document.rtrgHostBpm.load();
            return host > 0.0 ? "HOST " + juce::String(host, 0) : juce::String(v, 0);
        });
        bpm.slider.setRange(20.0, 300.0, 1.0);
        bpm.slider.setEnabled(standalone);

        syncCaption.setText("TIMING", juce::dontSendNotification);
        syncCaption.setJustificationType(juce::Justification::centred);
        syncCaption.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(syncCaption);
        syncButton.setClickingTogglesState(true);
        syncButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        syncButton.setTooltip("SYNC: TIME is a note value against the tempo. FREE: TIME is 10 ms to 1 s");
        syncButton.setToggleState(document.rtrgSync.load(), juce::dontSendNotification);
        syncButton.onClick = [this]
        {
            document.rtrgSync.store(syncButton.getToggleState());
            syncButtonText();
            time.slider.updateText();
        };
        addAndMakeVisible(syncButton);
        syncButtonText();

        setSize(300, 118);
        startTimerHz(15);
    }

    ~RetrigEditorPanel() override { setLookAndFeel(nullptr); }   // detach before lnf is destroyed

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        title.setBounds(r.removeFromTop(20));
        r.removeFromTop(4);
        const int w = r.getWidth() / 4;
        for (auto* k : { &time, &fade })
        {
            auto col = r.removeFromLeft(w);
            k->caption.setBounds(col.removeFromTop(14));
            k->slider.setBounds(col);
        }
        auto col = r.removeFromLeft(w);
        syncCaption.setBounds(col.removeFromTop(14));
        syncButton.setBounds(col.reduced(6, 12));
        bpm.caption.setBounds(r.removeFromTop(14));
        bpm.slider.setBounds(r);
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

    void syncButtonText() { syncButton.setButtonText(syncButton.getToggleState() ? "SYNC" : "FREE"); }

    void timerCallback() override
    {
        for (auto* k : { &time, &fade, &bpm })
        {
            if (k->slider.isMouseButtonDown())
                continue;
            const double v = k->target->load();
            if (std::abs(v - k->slider.getValue()) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
        const bool sync = document.rtrgSync.load();
        if (sync != syncButton.getToggleState())
        {
            syncButton.setToggleState(sync, juce::dontSendNotification);
            syncButtonText();
            time.slider.updateText();
        }
        const double host = document.rtrgHostBpm.load();
        if (host != shownHostBpm) { shownHostBpm = host; bpm.slider.updateText(); }
    }

    AudioDocument& document;
    const bool standalone;
    juce::Label title, syncCaption;
    Knob time, fade, bpm;
    juce::TextButton syncButton;
    double shownHostBpm = -1.0;
    lcd::HardwareLcdLookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetrigEditorPanel)
};

class RetrigPanel : public juce::Component,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    RetrigPanel(AudioDocument& doc, bool standaloneIn) : document(doc), standalone(standaloneIn)
    {
        pill.onClick = [this]
        {
            document.rtrgLatched.store(! document.rtrgLatched.load());
            syncPill();
        };
        pill.setTooltip("RTRG: click to latch the stutter on, click again to let go");
        addAndMakeVisible(pill);

        setUpKnob(timeKnob, "TIME", document.rtrgTime, 0.55,
                  [this](double v) { return rtrgText::time(v, document.rtrgSync.load()); });
        setUpKnob(fadeKnob, "FADE", document.rtrgFade, 0.5, [](double v) { return rtrgText::fade(v); });

        moreButton.setTooltip("Time / Fade / Sync-Free / BPM (double-click to pin)");
        moreButton.onClick = [this]
        {
            moreCallout.buttonClicked(moreButton, [this] { return std::make_unique<RetrigEditorPanel>(document, standalone); });
        };
        moreButton.setLookAndFeel(&moreButtonLnf);
        addAndMakeVisible(moreButton);

        applyTheme();
        theme->addChangeListener(this);
        startTimerHz(15);
    }

    ~RetrigPanel() override
    {
        moreCallout.close();
        timeKnob.slider.setLookAndFeel(nullptr);
        fadeKnob.slider.setLookAndFeel(nullptr);
        moreButton.setLookAndFeel(nullptr);
        theme->removeChangeListener(this);
    }

    void resized() override
    {
        // Same packing as ReverbPanel / PlexiphonPanel / MimeophonPanel (FxRow relies on it).
        auto r = getLocalBounds().reduced(4, 2);
        constexpr int gap = 3, pillW = 32, pillH = 16, knobW = 53, moreW = 20, moreH = 16;
        pill.setBounds(r.removeFromLeft(pillW).withSizeKeepingCentre(pillW, pillH));
        r.removeFromLeft(gap);
        for (auto* k : { &timeKnob, &fadeKnob })
        {
            auto kcol = r.removeFromLeft(knobW);
            k->caption.setBounds(kcol.removeFromTop(17));
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
            g.setFont(systemUIFont(10.0f));   // 4 letters in the same 32px pill as "RVB"
            g.drawText("RTRG", getLocalBounds(), juce::Justification::centred);
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
        const bool on = document.rtrgLatched.load();
        if (on != pill.on) { pill.on = on; pill.repaint(); }
    }

    void timerCallback() override
    {
        syncPill();
        for (auto* k : { &timeKnob, &fadeKnob })
        {
            const double v = k->target->load();
            if (! k->slider.isMouseButtonDown() && std::abs(k->slider.getValue() - v) > 1.0e-6)
                k->slider.setValue(v, juce::dontSendNotification);
        }
        const bool sync = document.rtrgSync.load();
        if (sync != shownSync) { shownSync = sync; timeKnob.slider.updateText(); }   // note name <-> ms
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }

    void applyTheme()
    {
        const auto& pal = theme->palette();
        pill.fill = pal.accent;
        pill.ink = pal.windowBg;
        pill.border = pal.textDim;
        pill.repaint();
        for (auto* k : { &timeKnob, &fadeKnob })
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
    const bool standalone;
    juce::SharedResourcePointer<ThemeManager> theme;
    Pill pill;
    Knob timeKnob, fadeKnob;
    bool shownSync = true;
    R3WRKLookAndFeel knobLnF;
    R3WRKIconOnlyLookAndFeel moreButtonLnf;
    juce::TextButton moreButton { R3WRKLookAndFeel::iconMore };
    PinnableCallout moreCallout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetrigPanel)
};
