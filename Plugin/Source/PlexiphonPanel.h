#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    The PLEXIPHON cell of the FX drawer (4th slot, replacing GRANULAR's old placeholder -- see
    FxRow::resized()): a real Plexiphon (PlexiphonEngine.h / PluginProcessor::applyPlexiphon()),
    not a placeholder Section. Two primary knobs -- Plexus (the central control per the manual)
    and Mix -- sit directly in the drawer, the same footprint the placeholder Section used to
    occupy there; a "..." button opens the full control set (Level, Plexus, Size, Diffuse,
    Decay, Color, Mix) in a popup, the same juce::CallOutBox pattern ReverbPanel/LfoPanel use
    for their own editors.
*/
class PlexiphonPanel : public juce::Component,
                       private juce::Timer,
                       private juce::ChangeListener
{
public:
    explicit PlexiphonPanel(AudioDocument& document);
    ~PlexiphonPanel() override;

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;   // low-rate re-sync from external changes (state load, undo)
    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }
    void applyTheme();
    void openFullEditor();

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;

    // Small enable pill -- same visual language as ReverbPanel/LfoPanel's own (each place draws
    // its own rather than sharing one component -- established precedent).
    struct EnablePill : juce::Component
    {
        juce::Colour fill, ink, border;
        bool on = false, hovered = false;
        std::function<void()> onClick;
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }
        void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hovered = false; repaint(); }
    };
    EnablePill enablePill;

    struct Knob
    {
        juce::Label caption;
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    };
    Knob plexusKnob, mixKnob;
    // plexusKnob/mixKnob's sliders are constructed as a PluginEditor member before its ctor body
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlexiphonPanel)
};
