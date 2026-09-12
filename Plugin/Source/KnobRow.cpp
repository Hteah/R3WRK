#include "KnobRow.h"
#include "BiquadFilter.h"   // r3wrk::mnm::{hpCutoffHz,lpCutoffHz} for the Base/Width knob readouts
#include <cmath>

namespace
{
    int64_t fracToSample(double frac, int64_t numSamples)
    {
        return (int64_t) (juce::jlimit(0.0, 1.0, frac) * (double) juce::jmax((int64_t) 1, numSamples));
    }

    juce::String filterHzText(double hz)
    {
        if (hz <= 0.0)      return "off";
        if (hz >= 19000.0)  return "open";
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

    //== Base (Monomachine multimode filter: high-pass corner) =============
    {
        auto& k = addKnob("Base");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setSkewFactorFromMidPoint(0.35);   // ~log Hz feel: fine control down low
        k.slider.setDoubleClickReturnValue(true, 0.0);   // 0 = no high-pass
        k.slider.textFromValueFunction = [this](double v) {
            return filterHzText(r3wrk::modelHpCutoffHz(
                (r3wrk::FilterModel) document.filterModel.load(), v));
        };
        k.slider.setValue(document.filterBase.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterBase.store(v); };
        k.pull  = [this] { return document.filterBase.load(); };
    }

    //== Width (low-pass corner, sits above Base at Base+Width) =============
    {
        auto& k = addKnob("Width");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 1.0);   // 1 = no low-pass (filter wide open)
        k.slider.textFromValueFunction = [this](double v) {
            return filterHzText(r3wrk::modelLpCutoffHz(
                (r3wrk::FilterModel) document.filterModel.load(), document.filterBase.load(), v));
        };
        k.slider.setValue(document.filterWidth.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterWidth.store(v); };
        k.pull  = [this] { return document.filterWidth.load(); };
    }

    //== HP Q (resonance at the high-pass edge) ===========================
    {
        auto& k = addKnob("HP Q");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.setValue(document.filterHpQ.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterHpQ.store(v); };
        k.pull  = [this] { return document.filterHpQ.load(); };
    }

    //== LP Q (resonance at the low-pass edge) ============================
    {
        auto& k = addKnob("LP Q");
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);
        k.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        k.slider.setValue(document.filterLpQ.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.filterLpQ.store(v); };
        k.pull  = [this] { return document.filterLpQ.load(); };
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
            if (onSelectionKnobMoved) onSelectionKnobMoved();
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
            if (onSelectionKnobMoved) onSelectionKnobMoved();
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

    addAndMakeVisible(modelBadge);
    modelBadge.onClick = [this]
    {
        document.filterModel.store((document.filterModel.load() + 1) % 2);
        syncModelBadge();
        for (auto* k : knobs) k->slider.updateText();   // Base/Width Hz readouts follow the model
    };

    applyTheme();
    syncModelBadge();
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
    modelBadge.fill   = pal.accent;
    modelBadge.ink    = pal.windowBg;
    modelBadge.border = pal.textDim;
    modelBadge.repaint();
}

void KnobRow::syncModelBadge()
{
    const bool ot = document.filterModel.load() == 1;
    if (modelBadge.active == ot)
        return;
    modelBadge.active = ot;
    modelBadge.text   = ot ? "OT" : "MNM";
    modelBadge.repaint();
    for (auto* k : knobs) k->slider.updateText();   // Base/Width readouts follow a model change from elsewhere
}

void KnobRow::ModelBadge::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    constexpr float rad = 3.0f;
    if (active)
        g.setColour(fill);
    else
        g.setColour(fill.withAlpha(hovered ? 0.22f : 0.0f));
    g.fillRoundedRectangle(r, rad);
    g.setColour(border.withAlpha(active || hovered ? 0.95f : 0.55f));
    g.drawRoundedRectangle(r, rad, 1.0f);
    g.setColour(active ? ink : border);
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    g.drawText(text, getLocalBounds(), juce::Justification::centred);
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
    // Cursor stays pinned to the knob for the whole drag -- see PinnedDragSlider's comment.

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
    syncModelBadge();   // pick up model changes from undo / state load
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
    const int gap      = 2;
    const int dotGap   = 16;   // the wider gap at a section-divider dot (before knob 7 / 9)
    const int badgeGap = 30;   // wider still before knob 3 -- the filter-model badge sits here
    const int n = juce::jmax(1, knobs.size());

    // Auto-fit: prefer a fairly tight column, but shrink further so every knob still shows at
    // the minimum window width (9-10: Pitch/Speed/Stretch/Base/Width/HP Q/LP Q/Start/End
    // [/Gain]). The rotary disc is sized off the column height, not its width, so a narrower
    // column just packs the knobs closer without shrinking them; the cap keeps the time
    // readouts (Start / End) from clipping.
    const int avail = juce::jmax(0, r.getWidth() - gap * (n - 1));
    const int knobW = juce::jlimit(46, 53, avail / n);

    for (int i = 0; i < knobs.size(); ++i)
    {
        if (r.getWidth() < 24) break;
        auto* k = knobs[i];
        auto col = r.removeFromLeft(juce::jmin(knobW, r.getWidth()));
        k->caption.setBounds(col.removeFromTop(14));
        k->slider.setBounds(col);

        // A wide gap at each section boundary (after knob 2 / 6 / 8 -- see paint()): the
        // filter-model badge before knob 3, plain divider dots before 7 / 9.
        const bool beforeBadge = (i == 2);
        const bool beforeDot   = (i == 6 || i == 8);
        r.removeFromLeft(beforeBadge ? badgeGap : (beforeDot ? dotGap : gap));
    }

    if (knobs.size() > 3)
    {
        const auto l  = knobs[2]->slider.getBounds();
        const auto rr = knobs[3]->slider.getBounds();
        if (rr.getX() > l.getRight())
        {
            const int bw = juce::jlimit(18, 28, rr.getX() - l.getRight() - 4);
            const int bh = 13;
            modelBadge.setBounds((l.getRight() + rr.getX()) / 2 - bw / 2,
                                 getHeight() / 2 - bh / 2, bw, bh);
        }
    }
    repaint();   // reposition the section dividers for the new knob width
}

void KnobRow::paint(juce::Graphics& g)
{
    // A small vertical run of three dots, centred in the gap before each of these knob
    // indices, marking the section boundaries: time/pitch | filter | selection | output gain.
    // Index 9 (Gain) only exists in the Standalone build.
    static constexpr int boundaryBefore[] = { 3 /*Base*/, 7 /*Start*/, 9 /*Gain*/ };

    const float cy = (float) getHeight() * 0.5f;
    constexpr int   count = 3;
    constexpr float radius = 1.5f, spacing = 5.0f;
    g.setColour(theme->palette().text.withAlpha(0.4f));

    for (int idx : boundaryBefore)
    {
        if (idx <= 0 || idx >= knobs.size())
            continue;
        if (idx == 3)
            continue;   // the filter-model badge occupies this boundary instead of dots
        const auto left  = knobs[idx - 1]->slider.getBounds();
        const auto right = knobs[idx]->slider.getBounds();
        if (right.getX() <= left.getRight())
            continue;   // not laid out yet
        const float x = (float) (left.getRight() + right.getX()) * 0.5f;
        float y = cy - (count - 1) * spacing * 0.5f;
        for (int i = 0; i < count; ++i, y += spacing)
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    }
}
