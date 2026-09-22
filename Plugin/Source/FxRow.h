#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoPanel.h"
#include "Theme.h"

/**
    The collapsible "FX drawer" beneath the main KnobRow, revealed by the chevron on its right
    edge (see PluginEditor::toggleFxDrawer(), which grows the window by the drawer's height
    rather than displacing anything already on screen).

    Two stacked rows: LFO + Delay on top, Reverb + Granular below -- giving each effect roughly
    double the width a single four-across row would allow, so there's room to grow past two
    knobs each without immediately running out of space again.

    LFO is real (see LfoPanel.h/.cpp and PluginProcessor::tickLfos()) -- it owns the whole LFO
    cell instead of the generic placeholder layout below. Delay/Reverb/Granular are still a
    design pass only: an enable pill + a pair of knobs in KnobRow's own visual language (rotary
    knobs pick up R3WRKLookAndFeel's theme-driven ring/pointer automatically -- no per-slider
    colour wiring needed), local to this component and not yet reaching AudioDocument.
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
        juce::String name;   // "LFO" / "DELAY" / "REVERB" / "GRANULAR"
        int row = 0;         // which of the two stacked rows this section lays out into
        EnablePill pill;
        Knob knobA, knobB;
        juce::Rectangle<int> outerBounds;   // for the divider drawn in paint()
        float dividerY = 0.0f;              // that row's vertical centre, for the divider dots
    };

    Section& addSection(int row, const juce::String& name,
                         const juce::String& captionA, const juce::String& captionB);
    void layoutOneSection(Section& s, juce::Rectangle<int> col);
    void layoutRow(juce::Rectangle<int> area, int row);

    juce::SharedResourcePointer<ThemeManager> theme;
    LfoPanel lfoPanel;               // row 0's first cell -- real, not a placeholder Section
    juce::OwnedArray<Section> sections;   // DELAY (row 0), REVERB + GRANULAR (row 1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
