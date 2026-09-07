#include "KnobRow.h"
#include "BiquadFilter.h"   // r3wrk::filterPosToHz for the Base/Width knob readouts
#include <cmath>

namespace
{
    int64_t fracToSample(double frac, int64_t numSamples)
    {
        return (int64_t) (juce::jlimit(0.0, 1.0, frac) * (double) juce::jmax((int64_t) 1, numSamples));
    }

    juce::String filterHzText(double hz)
    {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, hz < 10000.0 ? 2 : 1) + " kHz"
                            : juce::String(juce::roundToInt(hz)) + " Hz";
    }
}

KnobRow::KnobRow(AudioDocument& doc, bool standalone)
    : document(doc)
{
    //== Pitch =================================================================
    {
        auto& k = addKnob("Pitch");
        k.slider.setRange(AudioDocument::kMinPitch, AudioDocument::kMaxPitch, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);
        k.slider.textFromValueFunction = [](double v)
        {
            return (v > 0.0 ? "+" : "") + juce::String(v, 2) + " st";
        };
        k.slider.setValue(document.playbackPitch.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.playbackPitch.store(v); };
        k.pull  = [this] { return document.playbackPitch.load(); };
    }

    //== Speed (tape) =========================================================
    {
        auto& k = addKnob("Speed");
        k.slider.setRange(AudioDocument::kMinSpeed, AudioDocument::kMaxSpeed, 0.0);
        k.slider.setSkewFactorFromMidPoint(1.0);
        k.slider.setDoubleClickReturnValue(true, 1.0);
        k.slider.textFromValueFunction = [](double v) { return juce::String(v, 2) + juce::String::fromUTF8(" \xc3\x97"); }; // "×"
        k.slider.setValue(document.playbackSpeed.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.playbackSpeed.store(v); };
        k.pull  = [this] { return document.playbackSpeed.load(); };
    }

    //== Stretch (pure time-stretch, pitch kept) ==============================
    {
        auto& k = addKnob("Stretch");
        k.slider.setRange(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, 0.0);
        k.slider.setSkewFactorFromMidPoint(1.0);   // fine control near 1x, room to crank to 50x
        k.slider.setDoubleClickReturnValue(true, 1.0);
        k.slider.textFromValueFunction = [](double v)
        {
            return juce::String(v, v < 10.0 ? 2 : 1) + juce::String::fromUTF8(" \xc3\x97");
        };
        k.slider.setValue(document.playbackStretch.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.playbackStretch.store(v); };
        k.pull  = [this] { return document.playbackStretch.load(); };
    }

    //== Base (Octatrack-style filter: low edge / high-pass) ================
    {
        auto& k = addKnob("Base");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setSkewFactorFromMidPoint(0.35);   // ~log Hz feel: fine control down low
        k.slider.setDoubleClickReturnValue(true, 0.0);   // 0 = no high-pass
        k.slider.textFromValueFunction = [](double v) { return filterHzText(r3wrk::filterPosToHz(v)); };
        k.slider.setValue(document.filterBase.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterBase.store(v); };
        k.pull  = [this] { return document.filterBase.load(); };
    }

    //== Width (filter high edge / low-pass, sits above Base) ===============
    {
        auto& k = addKnob("Width");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 1.0);   // 1 = no low-pass (filter wide open)
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.setValue(document.filterWidth.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterWidth.store(v); };
        k.pull  = [this] { return document.filterWidth.load(); };
    }

    //== Q (filter resonance, applied at both edges) =======================
    {
        auto& k = addKnob("Q");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.setValue(document.filterResonance.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterResonance.store(v); };
        k.pull  = [this] { return document.filterResonance.load(); };
    }

    //== Start / End (selection edges) ========================================
    {
        auto& k = addKnob("Start");
        startKnob = &k;
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.textFromValueFunction = [this](double v)
        {
            return timeString(v * (double) document.getNumSamples()
                              / juce::jmax(1.0, document.getSampleRate()));
        };
        k.apply = [this](double v)
        {
            const int64_t n = juce::jmax((int64_t) 1, document.getNumSamples());
            const int64_t length = document.getSelectionEnd() - document.getSelectionStart();
            int64_t s = fracToSample(v, n);
            int64_t e;
            if (length > 0)
            {
                // Slide the whole window: End follows Start, keeping the selection length,
                // until it can't slide any further. Only the End knob changes the length.
                s = juce::jlimit((int64_t) 0, juce::jmax((int64_t) 0, n - length), s);
                e = s + length;
            }
            else
            {
                e = fracToSample(endKnob->slider.getValue(), n);
                if (e <= s) e = juce::jmin(n, s + 1);
            }
            document.setSelection(s, e);
        };
        k.pull = [this]
        {
            return (double) document.getSelectionStart()
                 / (double) juce::jmax((int64_t) 1, document.getNumSamples());
        };
    }
    {
        auto& k = addKnob("End");
        endKnob = &k;
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setValue(1.0, juce::dontSendNotification);
        k.slider.textFromValueFunction = [this](double v)
        {
            return timeString(v * (double) document.getNumSamples()
                              / juce::jmax(1.0, document.getSampleRate()));
        };
        k.apply = [this](double v)
        {
            const int64_t n = juce::jmax((int64_t) 1, document.getNumSamples());
            int64_t e = fracToSample(v, n);
            int64_t s = fracToSample(startKnob->slider.getValue(), n);
            if (s >= e) s = juce::jmax((int64_t) 0, e - 1);
            document.setSelection(s, e);
        };
        k.pull = [this]
        {
            return (double) document.getSelectionEnd()
                 / (double) juce::jmax((int64_t) 1, document.getNumSamples());
        };
    }

    //== Gain (Standalone only -- output level) ==============================
    if (standalone)
    {
        auto& k = addKnob("Gain");
        k.slider.setRange(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);   // 0 dB = default volume
        k.slider.textFromValueFunction = [](double db)
        {
            if (db <= AudioDocument::kMinGainDb + 0.05)
                return juce::String::fromUTF8("-\xE2\x88\x9E dB");   // "-∞ dB" (mute)
            return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
        };
        k.slider.setValue(document.playbackGainDb.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.playbackGainDb.store(v); };
        k.pull  = [this] { return document.playbackGainDb.load(); };
    }

    applyTheme();
    theme->addChangeListener(this);
    startTimerHz(15);
}

KnobRow::~KnobRow()
{
    for (auto* k : knobs)
        k->slider.setLookAndFeel(nullptr);   // detach before knobLnF is destroyed
    theme->removeChangeListener(this);
}

void KnobRow::applyTheme()
{
    const auto& pal = theme->palette();
    for (auto* k : knobs)
    {
        k->caption.setColour(juce::Label::textColourId, pal.textDim);
        k->slider.setColour(juce::Slider::rotarySliderFillColourId, pal.accent);
        k->slider.setColour(juce::Slider::textBoxTextColourId, pal.text);
        k->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k->caption.repaint();
        k->slider.repaint();
    }
}

void KnobRow::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
}

KnobRow::Knob& KnobRow::addKnob(const juce::String& name)
{
    auto* k = knobs.add(new Knob());

    k->caption.setText(name, juce::dontSendNotification);
    k->caption.setJustificationType(juce::Justification::centred);
    k->caption.setFont(juce::FontOptions(11.0f));

    k->slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 14);
    k->slider.setLookAndFeel(&knobLnF);

    Knob* kp = k;
    k->slider.onValueChange = [kp] { if (kp->apply) kp->apply(kp->slider.getValue()); };

    addAndMakeVisible(k->caption);
    addAndMakeVisible(k->slider);
    return *k;
}

juce::String KnobRow::timeString(double seconds) const
{
    seconds = juce::jmax(0.0, seconds);
    const int mins = (int) (seconds / 60.0);
    const double secs = seconds - mins * 60.0;
    return juce::String::formatted("%d:%06.3f", mins, secs);
}

void KnobRow::timerCallback()
{
    for (auto* k : knobs)
    {
        if (! k->pull || k->slider.isMouseButtonDown())
            continue;

        const double target = k->pull();
        if (std::abs(target - k->slider.getValue()) > 1.0e-6
            || std::abs(target - k->lastPulled) > 1.0e-9)
        {
            k->slider.setValue(target, juce::dontSendNotification);
            k->lastPulled = target;
        }
        k->slider.updateText();   // Start/End read out time, which depends on the clip length
    }
}

void KnobRow::resized()
{
    auto r = getLocalBounds().reduced(4, 2);
    const int gap = 6;
    const int n = juce::jmax(1, knobs.size());

    // Auto-fit: prefer 78 px, but shrink so every knob is shown even at the minimum window
    // width (8 knobs now: Pitch/Speed/Stretch/Base/Width/Q/Start/End).
    const int avail = juce::jmax(0, r.getWidth() - gap * (n - 1));
    const int knobW = juce::jlimit(46, 78, avail / n);

    for (auto* k : knobs)
    {
        if (r.getWidth() < 24) break;
        auto col = r.removeFromLeft(juce::jmin(knobW, r.getWidth()));
        k->caption.setBounds(col.removeFromTop(14));
        k->slider.setBounds(col);
        r.removeFromLeft(gap);
    }
}
