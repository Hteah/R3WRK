#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    The DELAY cell of the FX drawer, replacing its old placeholder Section: a real Mimeophon
    (MimeophonEngine.h / PluginProcessor::applyMimeophon()), not a placeholder. Two primary
    knobs -- Rate (the one you'd actually reach for while playing, the way you'd turn a real
    tape echo's delay-time knob live) and Mix -- sit directly in the drawer; a "..." button
    opens the full control set (Zone, Rate, Repeats, Color, Halo, Mix) in a popup, the same
    juce::CallOutBox pattern ReverbPanel/PlexiphonPanel/LfoPanel use for their own editors.
*/
class MimeophonPanel : public juce::Component,
                       private juce::Timer,
                       private juce::ChangeListener
{
public:
    explicit MimeophonPanel(AudioDocument& document);
    ~MimeophonPanel() override;

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;   // low-rate re-sync from external changes (state load, undo)
    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }
    void applyTheme();
    void openFullEditor();

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;

    // Small enable pill -- same visual language as ReverbPanel/PlexiphonPanel/LfoPanel's own
    // (each place draws its own rather than sharing one component -- established precedent).
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
    Knob rateKnob, mixKnob;
    // Boxless (see R3WRKIconOnlyLookAndFeel) -- the icon already has its own ring; the ordinary
    // transparent-background pill outline every other TextButton gets drew a second, redundant
    // oval around it.
    R3WRKIconOnlyLookAndFeel moreButtonLnf;
    juce::TextButton moreButton { R3WRKLookAndFeel::iconMore };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MimeophonPanel)
};
