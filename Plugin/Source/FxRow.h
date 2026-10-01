#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoPanel.h"
#include "ReverbPanel.h"
#include "PlexiphonPanel.h"
#include "MimeophonPanel.h"
#include "RetrigPanel.h"
#include "ChorusPanel.h"
#include "ShimmerPanel.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"
#include "LayoutTweaks.h"

/**
    The collapsible "FX drawer" beneath the main KnobRow, revealed by the chevron on its right
    edge (see PluginEditor::toggleFxDrawer(), which grows the window by the drawer's height
    rather than displacing anything already on screen).

    One row: RTRG-or-CHO | DLY-or-PLX | RVB-or-SHM | Gain, left to right (DLY is Mimeophon --
    see below; PLX moved here from the reverb slot in R3WY, SHM took its place). Each "-or-" slot
    shows one effect at a time, and only the shown one runs: the small tab above each pill
    switches the slot (the pills are the latch / the slot's on-off). LFO
    is still SHELVED, not deleted -- lfoPanel below is a real, fully wired member
    (PluginProcessor::tickLfos() still runs exactly as before; AudioDocument's lfoSlots fields
    still persist), just not addAndMakeVisible()'d or given a layout slot here, so it's a
    two-line change to bring back (see the "Add Erbe-Verb reverb, Plexiphon, and a hardware-LCD
    popup look" commit / its checkpoint tag for the last state with everything visible).
    Erbe-Verb (reverbPanel) was shelved alongside LFO earlier this session and is back now, per
    request, after Plexiphon.

    Plexiphon, Mimeophon, and Reverb are all real (PluginProcessor::applyPlexiphon()/
    applyMimeophon()/applyReverb()) -- each owns its whole cell. Mimeophon replaced the old
    placeholder DELAY Section (fake TIME/FDBK knobs, never wired to AudioDocument) -- fittingly,
    since Mimeophon literally is a delay -- so the generic placeholder-Section machinery that
    used to back it is gone too.

    A Granular playback mode (GranularEngine.h/GranularPanel) was built and then fully removed
    on request -- didn't land the way the user wanted, and they'd rather use Ableton's
    Granulator III directly than keep iterating on a clone. Nothing granular-related remains in
    this codebase (unlike LFO, which is shelved, not removed) -- a from-scratch build if it's
    ever wanted again, not a re-enable.
*/
class FxRow : public juce::Component,
              private juce::ChangeListener,
              private juce::Timer
{
public:
    FxRow(AudioDocument& document, bool standalone);
    ~FxRow() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    // Where the knob row's End knob sits (x range, this component's coordinates): the gaps
    // between DLY / PLX / RVB are spread evenly so Gain lands right under End at any width.
    void setGainColumn(juce::Range<int> col) { gainColumn = col; resized(); }

    // Where the first toggle's left edge goes (this component's coordinates): the left edge of
    // the knob row's Pitch disc, so the drawer lines up under it. -1 = not known yet.
    void setFirstEdge(int x) { firstEdge = x; resized(); }

    // The reset button (a red lightning bolt) sits right of the Gain knob, 12px from its disc.
    // onReset fires on click (the editor stops the transport + AudioDocument::resetSoundToDefaults).
    std::function<void()> onReset;

    // Edit Layout: the three slots + Gain, for LayoutEditOverlay.
    void getLayoutItems(juce::Array<LayoutItem>& items);

private:
    static constexpr int kToggleSlack = 60;   // how far left of its slot a toggle can be nudged
    static constexpr int kSpacing = 12;       // px between drawn items (not within a knob pair)
    static constexpr int kDotsW = 3;          // the divider dots' width (paint())
    // Lays one slot's panel out with its toggle text starting at x; returns where the next item
    // (after the divider dots) starts.
    template <typename Panel> int packSlot(Panel& panel, int x, const juce::String& id);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;   // Gain knob follows state loads / undo; the three shared slots follow their switches
    void applyTheme();

    juce::SharedResourcePointer<ThemeManager> theme;
    juce::SharedResourcePointer<LayoutTweaks> layout;   // Edit Layout nudges, applied in resized()
    LfoPanel lfoPanel;                // shelved -- see class comment; not shown, still fully wired
    RetrigPanel retrigPanel;          // 1st slot, shared with chorusPanel: only the selected
    ChorusPanel chorusPanel;          //   one shows (the tab above its pill switches RTRG <-> CHO)
    void syncModSlot();
    MimeophonPanel mimeoPanel;        // 2nd slot, shared with plexPanel: only the selected
    PlexiphonPanel plexPanel;         //   model's panel shows (the tab above the pill switches)
    void syncDelaySlot();
    ReverbPanel reverbPanel;          // 3rd slot, shared with shimmerPanel, same scheme
    ShimmerPanel shimmerPanel;
    void syncSpaceSlot();

    // Gain -- the output volume, last in the whole chain (after every effect, tails included;
    // see PluginProcessor::processBlock). Every build (Standalone-only until 2026-09-30); moved here from the
    // end of KnobRow to make room for Dirt. Styled like KnobRow's knobs.
    AudioDocument& document;
    const bool showGain;
    juce::Range<int> gainColumn;   // empty until PluginEditor passes it
    int firstEdge = -1;            // Pitch disc's left edge, -1 until PluginEditor passes it
    R3WRKIconOnlyLookAndFeel resetLnF;
    juce::TextButton resetButton { R3WRKLookAndFeel::iconReset };
    R3WRKLookAndFeel gainLnF;
    juce::Label gainCaption;
    juce::Slider gainKnob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
