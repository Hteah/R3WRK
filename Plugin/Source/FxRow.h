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

    One row: LFO | Delay | Reverb | Plexiphon, left to right (Plexiphon replaced Granular's old
    placeholder slot). LFO/Reverb/Plexiphon moved their real controls behind a popup editor
    instead of needing inline space -- just a compact summary sits in the drawer itself (see
    LfoPanel.h/.cpp, ReverbPanel.h/.cpp, PlexiphonPanel.h/.cpp).

    LFO, Reverb, and Plexiphon are real (PluginProcessor::tickLfos()/applyReverb()/
    applyPlexiphon()) -- each owns its whole cell instead of the generic placeholder layout
    below. Delay is still a design pass only: an enable pill + a pair of knobs in KnobRow's own
    visual language (rotary knobs pick up R3WRKLookAndFeel's theme-driven ring/pointer
    automatically -- no per-slider colour wiring needed), local to this component and not yet
    reaching AudioDocument.
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
    LfoPanel lfoPanel;               // 1st slot -- real, not a placeholder Section
    ReverbPanel reverbPanel;         // 3rd slot -- real, not a placeholder Section
    PlexiphonPanel plexPanel;        // 4th slot -- real, not a placeholder Section
    juce::OwnedArray<Section> sections;   // DELAY (2nd slot)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
