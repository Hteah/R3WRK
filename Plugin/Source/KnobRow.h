#pragma once
#include <JuceHeader.h>
#include <limits>
#include "AudioDocument.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

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
    // `standalone` -- the app build, not a plugin -- gets the extra output "Gain" knob.
    KnobRow(AudioDocument& document, bool standalone);
    ~KnobRow() override;

    // Fired while the Start / End knobs slide the selection, so the editor can scroll the
    // waveform view to keep the selection markers on screen when zoomed in.
    std::function<void()> onSelectionKnobMoved;

    void resized() override;
    void paint(juce::Graphics&) override;   // faint group dividers between the knob sections

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;   // theme changed
    void applyTheme();

    // A rotary Slider that pins the cursor to itself for the whole drag gesture, instead of
    // letting the real cursor travel across the screen.
    //
    // Plain Slider only offers two drag modes, and neither is quite right for a small knob
    // sitting above other draggable UI (here, the waveform): absolute mode (the default) maps
    // *distance* from the click point to value -- precise and direct, but with a real,
    // visibly-travelling cursor that a big knob swing (JUCE's default full-range drag extent
    // is ~250px) easily carries off the knob and onto whatever's underneath; velocity mode
    // hides the cursor, but maps drag *speed* to value instead of distance, which feels
    // comparatively vague and laggy for careful adjustments -- turning down a filter knob
    // slowly barely registers, then a slightly faster nudge overshoots.
    //
    // This gets both: enabling unbounded mouse movement doesn't change how Slider computes
    // the drag at all -- it only changes what MouseEvent::position *reports*. Once engaged,
    // JUCE keeps the OS cursor pinned near the component (hidden, warped back to centre
    // whenever it would leave) while accumulating the movement that would have happened, and
    // hands that accumulated position to mouseDrag instead of the pinned real one. Slider's
    // own absolute-drag math reads that position exactly as before, so the feel -- and the
    // skew/interval/rotary-wrap handling that math already does correctly -- is untouched;
    // only the cursor's visible travel disappears.
    struct PinnedDragSlider : public juce::Slider
    {
        using juce::Slider::Slider;

        void mouseDown(const juce::MouseEvent& e) override
        {
            juce::Slider::mouseDown(e);
            if (isMouseButtonDown())
                e.source.enableUnboundedMouseMovement(true, false);
        }
    };

    struct Knob
    {
        juce::Label       caption;
        PinnedDragSlider  slider;
        std::function<void(double)> apply;   // slider value -> model  (user turns only)
        std::function<double()>     pull;    // model -> slider value  (external changes)
        double lastPulled = std::numeric_limits<double>::quiet_NaN();
    };

    // The filter-model switch: a small clickable pill sitting where the knob row's
    // filter-section divider dots would be (before the "Base" knob). Shows "MNM" / "OT".
    struct ModelBadge : juce::Component
    {
        juce::String text { "MNM" };
        bool active = false;                       // non-default model -> accent fill
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

    Knob& addKnob(const juce::String& name);
    juce::String timeString(double seconds) const;
    void syncModelBadge();   // text/colour/tooltip from document.filterModel

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    R3WRKLookAndFeel knobLnF;

    juce::OwnedArray<Knob> knobs;
    Knob* startKnob = nullptr;
    Knob* endKnob   = nullptr;
    ModelBadge modelBadge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobRow)
};
