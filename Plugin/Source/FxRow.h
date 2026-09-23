#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoPanel.h"
#include "ReverbPanel.h"
#include "PlexiphonPanel.h"
#include "Theme.h"

/**
    The collapsible "FX drawer" beneath the main KnobRow, revealed by the chevron on its right
    edge (see PluginEditor::toggleFxDrawer(), which grows the window by the drawer's height
    rather than displacing anything already on screen).

    One row: Delay | Plexiphon, left to right. LFO and Reverb are SHELVED, not deleted -- their
    panels (lfoPanel/reverbPanel below) are still real, fully wired members (PluginProcessor::
    tickLfos()/applyReverb() still run exactly as before; AudioDocument's lfoSlots/reverbXxx
    fields still persist), just not addAndMakeVisible()'d or given a layout slot here, so they're
    a two-line change to bring back (see the "Add Erbe-Verb reverb, Plexiphon, and a hardware-LCD
    popup look" commit / its checkpoint tag for the last state with both visible). The user
    preferred Plexiphon over Erbe-Verb and wanted a simpler drawer for now.

    Plexiphon is real (PluginProcessor::applyPlexiphon()) -- owns its whole cell instead of the
    generic placeholder layout below. Delay is still a design pass only: an enable pill + a pair
    of knobs in KnobRow's own visual language (rotary knobs pick up R3WRKLookAndFeel's theme-
    driven ring/pointer automatically -- no per-slider colour wiring needed), local to this
    component and not yet reaching AudioDocument.
*/
class FxRow : public juce::Component,
              private juce::ChangeListener
{
public:
    FxRow(AudioDocument& document, bool standalone);
    ~FxRow() override;

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void applyTheme();

    // Small enable/bypass toggle per effect -- same rounded-pill shape as KnobRow's filter-model
    // badge, generalised to a plain on/off rather than a 2-way model switch.
    struct EnablePill : juce::Component
    {
        juce::String text;
        bool on = false;
        juce::Colour fill, ink, border;
        std::function<void()> onClick;

        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (onClick && getLocalBounds().toFloat().contains(e.position)) onClick();
        }
        void mouseEnter(const juce::MouseEvent&) override { hovered = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
        bool hovered = false;
    };

    struct Knob
    {
        juce::Label  caption;
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    };

    struct Section
    {
        juce::String name;   // "DELAY"
        EnablePill pill;
        Knob knobA, knobB;
        juce::Rectangle<int> outerBounds;   // for the divider drawn in paint()
    };

    Section& addSection(const juce::String& name,
                         const juce::String& captionA, const juce::String& captionB);
    void layoutOneSection(Section& s, juce::Rectangle<int> col);

    juce::SharedResourcePointer<ThemeManager> theme;
    LfoPanel lfoPanel;                // shelved -- see class comment; not shown, still fully wired
    ReverbPanel reverbPanel;          // shelved -- see class comment; not shown, still fully wired
    PlexiphonPanel plexPanel;         // 2nd (last) slot -- real, not a placeholder Section
    juce::OwnedArray<Section> sections;   // DELAY (1st slot)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
