#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"

/**
    The REVERB cell of the FX drawer (row 1, left half -- mirrors how LfoPanel owns row 0's left
    half, see FxRow::resized()): a real Erbe-Verb reverb (ReverbEngine.h / PluginProcessor::
    applyReverb()), not a placeholder Section. Two primary knobs (Decay, Mix -- the two you'd
    reach for most while playing) sit directly in the drawer, in the same footprint the
    placeholder Section used to occupy there; a "..." button opens the full control set (Size,
    Absorb, Decay, Tilt, Mix, Pre-delay) in a popup, the same juce::CallOutBox pattern
    LfoPanel::openEditorFor() uses for its own per-instance editor.
*/
class ReverbPanel : public juce::Component,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    explicit ReverbPanel(AudioDocument& document);
    ~ReverbPanel() override;

    void resized() override;
    void paint(juce::Graphics&) override;

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
    Knob decayKnob, mixKnob;
    juce::TextButton moreButton { "..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbPanel)
};
