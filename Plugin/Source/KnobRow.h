#pragma once
#include <JuceHeader.h>
#include <limits>
#include "AudioDocument.h"
#include "Theme.h"

/**
    A horizontal strip of small rotary knobs beneath the transport bar.

    Starter set: Pitch (semitones), Speed (tape rate), Start / End (selection edges).
    Extensible — add a knob with addKnob("Name") in the constructor and give it an
    apply (slider -> model) and, optionally, a pull (model -> slider, so the knob
    follows changes made elsewhere, e.g. dragging the selection brackets).

    Talks to the AudioDocument directly, the same way EditorToolbar does (the playback
    knobs live on AudioDocument -- see its "Live playback knobs" section -- so the views
    can read them too, not just the audio thread).
*/
class KnobRow : public juce::Component,
                private juce::Timer,
                private juce::ChangeListener
{
public:
    explicit KnobRow(AudioDocument& document);
    ~KnobRow() override;

    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;   // theme changed
    void applyTheme();

    struct Knob
    {
        juce::Label  caption;
        juce::Slider slider;
        std::function<void(double)> apply;   // slider value -> model  (user turns only)
        std::function<double()>     pull;    // model -> slider value  (external changes)
        double lastPulled = std::numeric_limits<double>::quiet_NaN();
    };

    Knob& addKnob(const juce::String& name);
    juce::String timeString(double seconds) const;

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;

    juce::OwnedArray<Knob> knobs;
    Knob* startKnob = nullptr;
    Knob* endKnob   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobRow)
};
