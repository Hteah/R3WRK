#pragma once
#include <JuceHeader.h>
#include <limits>
#include "AudioDocument.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"
#include "PinnableCallout.h"
#include "SlotSwitchTab.h"
#include "LayoutTweaks.h"

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
    // (The Standalone-only Gain knob moved to the end of the FX drawer -- FxRow -- to make room
    // for Dirt, so the row no longer needs to know which build it's in.)
    explicit KnobRow(AudioDocument& document);
    ~KnobRow() override;

    // Fired while the Start / End knobs slide the selection, so the editor can scroll the
    // waveform view to keep the selection markers on screen when zoomed in.
    std::function<void()> onSelectionKnobMoved;

    // Fired on a click of the drawer chevron at the row's right edge. The editor owns whether
    // the FX drawer is actually open (it resizes the window), so this only reports the click;
    // call setDrawerOpen() back once the editor has decided the new state.
    std::function<void()> onDrawerToggle;

    // Fired by the reset button under the drawer toggle (the boxed X): the editor stops the
    // transport and puts every knob / effect back to default (AudioDocument::resetSoundToDefaults).
    std::function<void()> onReset;
    void setDrawerOpen(bool open);
    void setDrawerToggleVisible(bool v) { drawerButton.setVisible(v); resetButton.setVisible(v); }   // hidden in Edit Layout mode

    void resized() override;
    void paint(juce::Graphics&) override;   // faint group dividers between the knob sections

    // The last knob's (End's) x range, in this component's coordinates -- the FX drawer spaces
    // its effects so its Gain knob sits right under it (FxRow::setGainColumn).
    juce::Range<int> getLastColumnXRange() const
    {
        if (knobs.isEmpty()) return {};
        const auto b = knobs.getLast()->slider.getBounds();
        return { b.getX(), b.getRight() };
    }

    // The left edge of the first knob's (Pitch's) drawn disc, in this component's coordinates --
    // the FX drawer lines its first toggle up under it (FxRow::setFirstEdge). Mirrors
    // R3WRKLookAndFeel::drawRotarySlider: a circle inset 3px, centred in the slider's rotary area.
    int getFirstKnobDiscLeft() const
    {
        if (knobs.isEmpty()) return 0;
        auto& s = knobs.getFirst()->slider;
        const auto rotary = s.getLookAndFeel().getSliderLayout(const_cast<PinnedDragSlider&>(s)).sliderBounds.toFloat().reduced(3.0f);
        const float diameter = juce::jmin(rotary.getWidth(), rotary.getHeight());
        return s.getX() + juce::roundToInt(rotary.getCentreX() - diameter * 0.5f);
    }

    // Edit Layout: every movable item (each knob, the filter badge), for LayoutEditOverlay.
    void getLayoutItems(juce::Array<LayoutItem>& items);

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
    struct ModelBadge : juce::Component, public juce::SettableTooltipClient
    {
        juce::String text { "MNM" };
        bool active = false;                       // the filter is on (AudioDocument::filterOn)
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
    void syncModelBadge();   // badge text + on/off and the tab's target, from filterModel / filterOn

    // Readout sizing/font now live on the base class itself (R3WRKLookAndFeel::
    // createSliderTextBox), so every rotary knob using that look matches KnobRow's without this
    // subclass overriding anything -- kept only as a distinctly-named instance for the
    // setLookAndFeel/nullptr attach-detach pairing below.
    class KnobLookAndFeel : public R3WRKLookAndFeel {};

    AudioDocument& document;
    juce::SharedResourcePointer<ThemeManager> theme;
    juce::SharedResourcePointer<LayoutTweaks> layout;   // Edit Layout nudges, applied in resized()
    KnobLookAndFeel knobLnF;

    juce::OwnedArray<Knob> knobs;
    Knob* startKnob = nullptr;
    Knob* endKnob   = nullptr;
    ModelBadge modelBadge;       // the filter's on/off, named for the model (dot-matrix MNM / OT)
    SlotSwitchTab modelTab;      // above it: switches the filter model, like the FX slots' tab

    // Dirt's popup (Drive / Rate / Bits -- DirtPanel.h): the same small "more" dot the FX
    // panels use, in its own slot right of the Dirt knob. One click opens, double-click pins (PinnableCallout);
    // the knob itself keeps double-click-to-reset like every other knob in the row.
    Knob* dirtKnob = nullptr;
    R3WRKIconOnlyLookAndFeel dirtMoreLnF;
    juce::TextButton dirtMoreButton { R3WRKLookAndFeel::iconMore };
    PinnableCallout dirtCallout;

    // FX drawer toggle -- the orbit icon, pinned to the row's right edge. Boxless (see
    // R3WRKIconOnlyLookAndFeel) per the user's request -- no permanent frame around it, only a
    // faint wash on hover/press/while the drawer is open.
    R3WRKIconOnlyLookAndFeel drawerLnF;
    juce::TextButton drawerButton { R3WRKLookAndFeel::iconOrbit };
    juce::TextButton resetButton  { R3WRKLookAndFeel::iconReset };   // under it -- see onReset

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobRow)
};
