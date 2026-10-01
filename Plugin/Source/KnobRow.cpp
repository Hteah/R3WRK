#include "KnobRow.h"
#include "BiquadFilter.h"   // r3wrk::mnm::{hpCutoffHz,lpCutoffHz} for the Base/Width knob readouts
#include "DirtPanel.h"
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

KnobRow::KnobRow(AudioDocument& doc)
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

    //== Dirt (pre-filter drive; Rate / Bits in its popup) ====================
    {
        auto& k = addKnob("Dirt");
        dirtKnob = &k;
        k.slider.setRange(0.0, 1.0, 0.0);
        k.slider.setDoubleClickReturnValue(true, 0.0);   // 0 = no drive
        k.slider.textFromValueFunction = [](double v)
        {
            return v < 0.005 ? juce::String("off") : juce::String(juce::roundToInt(v * 100.0)) + "%";
        };
        k.slider.setValue(document.dirtDrive.load(), juce::dontSendNotification);
        k.slider.updateText();
        k.apply = [this](double v) { document.dirtDrive.store(v); };
        k.pull  = [this] { return document.dirtDrive.load(); };
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
        // Playback chases this edge for as long as it's set -- see selectionEdgeDragging's
        // comment on AudioDocument. Covers a drag started on the knob itself; the waveform's
        // own Start/End brackets set the same flag from WaveformDisplay's mouse handling.
        k.slider.onDragStart = [this] { document.selectionEdgeDragging = 1; };
        k.slider.onDragEnd   = [this] { document.selectionEdgeDragging = 0; };
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
        k.slider.onDragStart = [this] { document.selectionEdgeDragging = 2; };
        k.slider.onDragEnd   = [this] { document.selectionEdgeDragging = 0; };
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

    // The badge is the filter's on/off (off glides it open and bypasses; the knobs keep their
    // settings); the small tab above it switches the model, like the FX drawer slots' tab.
    addAndMakeVisible(modelBadge);
    modelBadge.onClick = [this]
    {
        document.filterOn.store(! document.filterOn.load());
        syncModelBadge();
    };
    addAndMakeVisible(modelTab);
    modelTab.onClick = [this]
    {
        document.filterModel.store((document.filterModel.load() + 1) % 2);
        syncModelBadge();
    };

    dirtMoreButton.setTooltip("Dirt: Drive / Rate / Bits (double-click to pin)");
    dirtMoreButton.setWantsKeyboardFocus(false);
    dirtMoreButton.setLookAndFeel(&dirtMoreLnF);
    dirtMoreButton.onClick = [this]
    {
        dirtCallout.buttonClicked(dirtMoreButton, [this] { return std::make_unique<DirtPanel>(document); });
    };
    addAndMakeVisible(dirtMoreButton);

    drawerButton.setClickingTogglesState(false);   // the editor decides open/closed, not the button itself
    drawerButton.setWantsKeyboardFocus(false);
    drawerButton.setTooltip("More effects (Retrig / Chorus, Delay / PLX, Reverb / Shimmer)");
    drawerButton.setLookAndFeel(&drawerLnF);
    drawerButton.onClick = [this] { if (onDrawerToggle) onDrawerToggle(); };
    addAndMakeVisible(drawerButton);

    applyTheme();
    syncModelBadge();
    theme->addChangeListener(this);
    startTimerHz(15);
}

KnobRow::~KnobRow()
{
    for (auto* k : knobs)
        k->slider.setLookAndFeel(nullptr);   // detach before knobLnF is destroyed
    drawerButton.setLookAndFeel(nullptr);
    dirtCallout.close();
    dirtMoreButton.setLookAndFeel(nullptr);   // detach before dirtMoreLnF is destroyed
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
    modelBadge.fill   = pal.text;                                             // on ink
    modelBadge.ink    = pal.windowBg;   // (unused by the bracket toggle)
    modelBadge.border = pal.textDim;                                          // off ink
    modelBadge.repaint();
    modelTab.ink    = pal.textDim;
    modelTab.border = pal.textDim;
    modelTab.repaint();

    // Same outline idiom as the header's follow / float-on-top buttons: transparent fill,
    // plain text ink.
    drawerButton.setColour(juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    drawerButton.setColour(juce::TextButton::textColourOffId,  pal.text);
    // Same text ink when open too (per the user -- accent read as dimmed in themes with a muted
    // accent); the glyph itself shows the state, dropping its outer orbit ring once open.
    drawerButton.setColour(juce::TextButton::textColourOnId,   pal.text);

    dirtMoreButton.setColour(juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
    dirtMoreButton.setColour(juce::TextButton::textColourOffId, pal.textDim);
    dirtMoreButton.repaint();
}

void KnobRow::setDrawerOpen(bool open)
{
    drawerButton.setToggleState(open, juce::dontSendNotification);
}

void KnobRow::syncModelBadge()
{
    const bool ot = document.filterModel.load() == 1;
    const bool on = document.filterOn.load();
    const juce::String name = ot ? "OT" : "MNM";
    if (modelBadge.text != name)
    {
        modelBadge.text = name;
        modelTab.target = ot ? "MNM" : "OT";
        modelTab.setTooltip("Switch the filter to the " + juce::String(ot ? "Monomachine (MNM)" : "Octatrack (OT)") + " model");
        modelTab.repaint();
        for (auto* k : knobs) k->slider.updateText();   // Base/Width Hz readouts follow the model
    }
    if (modelBadge.active != on || modelBadge.text != name)
    {
        modelBadge.active = on;
        modelBadge.repaint();
    }
    modelBadge.setTooltip("Filter " + name + ": click to switch on/off (off keeps the knob settings)");
}

void KnobRow::ModelBadge::paint(juce::Graphics& g)
{
    // Corner brackets, like the FX drawer toggles: fill = on ink, border = off ink (see applyTheme()).
    drawBracketToggle(g, getLocalBounds().toFloat(), text, active, hovered, fill, border);
}

void KnobRow::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyTheme();
}

KnobRow::Knob& KnobRow::addKnob(const juce::String& name)
{
    auto* k = knobs.add(new Knob());

    k->caption.setText(name.toUpperCase(), juce::dontSendNotification);   // trying all-caps captions
    k->caption.setJustificationType(juce::Justification::centred);
    k->caption.setFont(juce::FontOptions(14.0f));

    k->slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
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

    // Drawer toggle (the orbit icon): pinned to the right edge, vertically centred, ahead of
    // the knob layout below so it never competes with the auto-fit knob width. Sized up from
    // the old chevron's 20px -- the orbit glyph (rings + dots) reads as a smudge that small.
    constexpr int drawerW = 46;   // a touch under the knobs' own auto-fit width (46-53px) --
                                  // the orbit icon's outer ring already fills most of this box
                                  // (see drawOrbitIcon's fillFraction), so this reads slightly
                                  // smaller than a knob's own disc, not bigger.
    auto drawerArea = r.removeFromRight(drawerW);
    drawerButton.setBounds(drawerArea.withSizeKeepingCentre(drawerW, drawerW));
    r.removeFromRight(6);

    const int gap      = 2;
    const int dotGap   = 16;   // the wider gap at the section-divider dot (before knob 8, Start)
    const int badgeGap = 60;   // wider still before knob 3 -- the filter-model badge sits here
    const int n = juce::jmax(1, knobs.size());

    // Auto-fit: prefer a fairly tight column, but shrink further so every knob still shows at
    // the minimum window width (10: Pitch/Speed/Stretch/Dirt/Base/Width/HP Q/LP Q/Start/End). The rotary disc is sized off the column height, not its width, so a narrower
    // column just packs the knobs closer without shrinking them; the cap keeps the time
    // readouts (Start / End) from clipping.
    // Dirt's "more" dot sits in its own slot right of the Dirt knob, at the FX drawer's
    // "more"-dot size -- reserved here so the knobs auto-fit around it.
    constexpr int moreW = 20, moreH = 16;
    const int dirtIndex = dirtKnob != nullptr ? knobs.indexOf(dirtKnob) : -1;
    const int sectionExtra = (n > 3 ? badgeGap - gap : 0) + (n > 8 ? dotGap - gap : 0);   // the wide gaps take their share too
    const int avail = juce::jmax(0, r.getWidth() - gap * (n - 1) - sectionExtra - (dirtIndex >= 0 ? moreW + gap : 0));
    const int knobW = juce::jlimit(46, 53, avail / n);

    for (int i = 0; i < knobs.size(); ++i)
    {
        if (r.getWidth() < 24) break;
        auto* k = knobs[i];
        auto col = r.removeFromLeft(juce::jmin(knobW, r.getWidth()));
        k->caption.setBounds(col.removeFromTop(17));
        k->slider.setBounds(col);

        // A wide gap at each section boundary (after knob 2 / 7 -- see paint()): the
        // filter-model badge before knob 3 (Dirt, first of the filter section), divider dots
        // before 8 (Start).
        const bool beforeBadge = (i == 2);
        const bool beforeDot   = (i == 7);
        r.removeFromLeft(beforeBadge ? badgeGap : (beforeDot ? dotGap : gap));

        if (i == dirtIndex)
        {
            auto slot = r.removeFromLeft(juce::jmin(moreW, r.getWidth()));
            dirtMoreButton.setBounds(slot.getX(), getHeight() / 2 - moreH / 2, moreW, moreH);
            r.removeFromLeft(gap);
        }
    }

    if (knobs.size() > 3)
    {
        const auto l  = knobs[2]->slider.getBounds();
        const auto rr = knobs[3]->slider.getBounds();
        if (rr.getX() > l.getRight())
        {
            const int bw = juce::jlimit(20, 58, rr.getX() - l.getRight() - 2);
            const int bh = 22;
            modelBadge.setBounds((l.getRight() + rr.getX()) / 2 - bw / 2,
                                 getHeight() / 2 - bh / 2, bw, bh);
            // The model tab sits in the caption row above the badge, like the FX slots' tab.
            const auto cap = knobs[3]->caption.getBounds();
            modelTab.setBounds((l.getRight() + rr.getX()) / 2 - bw / 2, cap.getCentreY() - 13 / 2, bw, 13);
        }
    }

    // The user's Edit Layout nudges (LayoutTweaks), on top of the automatic layout above. The
    // badge was placed from the unshifted knobs first, so moving a neighbour doesn't drag it.
    if (const int dx = layout->get("badge.filter"); dx != 0)
    {
        modelBadge.setTopLeftPosition(modelBadge.getPosition().translated(dx, 0));
        modelTab.setTopLeftPosition(modelTab.getPosition().translated(dx, 0));
    }
    for (auto* k : knobs)
    {
        const int dx = layout->get("knob." + k->caption.getText());
        if (dx == 0) continue;
        k->caption.setTopLeftPosition(k->caption.getPosition().translated(dx, 0));
        k->slider.setTopLeftPosition(k->slider.getPosition().translated(dx, 0));
        if (k == dirtKnob)
            dirtMoreButton.setTopLeftPosition(dirtMoreButton.getPosition().translated(dx, 0));
    }
    repaint();   // reposition the section dividers for the new knob width
}

void KnobRow::getLayoutItems(juce::Array<LayoutItem>& items)
{
    for (auto* k : knobs)
    {
        LayoutItem it { "knob." + k->caption.getText(), this,
                        k->caption.getBounds().getUnion(k->slider.getBounds()), {} };
        if (k == dirtKnob)
            it.bounds = it.bounds.getUnion(dirtMoreButton.getBounds());
        it.guides.add(layoutGuideRect(k->slider));
        items.add(it);
    }
    if (modelBadge.isVisible() && ! modelBadge.getBounds().isEmpty())
        items.add({ "badge.filter", this, modelBadge.getBounds().getUnion(modelTab.getBounds()),
                    { toggleInkBounds(modelBadge.getBounds(), modelBadge.text) } });
}

void KnobRow::paint(juce::Graphics& g)
{
    // A small vertical run of three dots, centred in the gap before each of these knob
    // indices, marking the section boundaries: time/pitch | Dirt + filter | selection. (Gain
    // moved to the end of the FX drawer.)
    static constexpr int boundaryBefore[] = { 3 /*Dirt*/, 8 /*Start*/ };

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
