#pragma once
#include <JuceHeader.h>
#include "PinnableCallout.h"
#include "AudioDocument.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"
#include "SlotSwitchTab.h"

/**
    The SHM cell of the FX drawer: the clean shimmer reverb (ShimmerEngine.h / PluginProcessor::
    applyShimmer()), sharing the last slot with RVB (ShimmerPanel -- this is a copy of it, kept
    separate per the drawer's one-panel-per-effect pattern). Decay + Mix in the drawer; the
    "more" dot opens all nine controls (Size, Decay, Tone, Width, Mix, Shimmer, Interval,
    Movement, Freeze).
*/
class ShimmerPanel : public juce::Component,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    explicit ShimmerPanel(AudioDocument& document);
    ~ShimmerPanel() override;

    void resized() override;
    void paint(juce::Graphics&) override;


    // Edit Layout (FxRow): room left of the toggle column it can be nudged into, and the parts
    // that move as "the toggle".
    int leftSlack = 0;
    juce::Array<juce::Component*> getToggleParts() { return { &enablePill, &slotTab }; }
    juce::String getToggleText() const { return "SHM"; }

private:
    void timerCallback() override;   // low-rate re-sync from external changes (state load, undo)
    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }
    void applyTheme();
    void openFullEditor();

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;

    // Small enable pill -- same rounded-pill visual language as FxRow::EnablePill / LfoPanel::
    // Row's own pill; each place draws its own rather than sharing one component (established
    // precedent -- see LfoPanel::Row::paint).
    struct EnablePill : juce::Component, public juce::SettableTooltipClient
    {
        juce::Colour fill, ink, border;
        bool on = false, hovered = false;
        std::function<void()> onClick;
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }
        void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hovered = false; repaint(); }
    };
    EnablePill enablePill;       // the RVB/SHM slot's on/off (AudioDocument::spaceOn)
    SlotSwitchTab slotTab;       // above the pill: switches the slot to the other model

    struct Knob
    {
        juce::Label caption;
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    };
    Knob decayKnob, mixKnob;
    // decayKnob/mixKnob's sliders are constructed as a PluginEditor member before its ctor body
    // installs fontLnf as the app-wide default LookAndFeel (see PluginEditor.cpp), so each
    // slider's textbox Label would otherwise get created (and cached) against JUCE's own stock
    // default rather than R3WRKLookAndFeel's Space Mono override -- explicit attach, same
    // pattern KnobRow's own knobs already use, makes the readout font match regardless of that
    // construction-order timing.
    R3WRKLookAndFeel knobLnF;
    // Boxless (see R3WRKIconOnlyLookAndFeel) -- the icon already has its own ring; the ordinary
    // transparent-background pill outline every other TextButton gets drew a second, redundant
    // oval around it.
    R3WRKIconOnlyLookAndFeel moreButtonLnf;
    juce::TextButton moreButton { R3WRKLookAndFeel::iconMore };
    PinnableCallout moreCallout;   // the full editor popup -- see PinnableCallout.h

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShimmerPanel)
};
