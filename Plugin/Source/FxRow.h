#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoPanel.h"
#include "ReverbPanel.h"
#include "PlexiphonPanel.h"
#include "MimeophonPanel.h"
#include "RetrigPanel.h"
#include "ChorusPanel.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"
#include "LayoutTweaks.h"

/**
    The collapsible "FX drawer" beneath the main KnobRow, revealed by the chevron on its right
    edge (see PluginEditor::toggleFxDrawer(), which grows the window by the drawer's height
    rather than displacing anything already on screen).

    One row: RTRG-or-CHO | Delay | Reverb-or-Plexiphon | Gain, left to right (Delay is Mimeophon
    -- see below). Both "-or-" slots show one effect at a time, and only the shown one runs:
    RVB/PLX switch with their pill, RTRG/CHO with the small tab above theirs (their pills are the
    latch / the on-off). LFO
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

    // Edit Layout: the three slots + Gain, for LayoutEditOverlay.
    void getLayoutItems(juce::Array<LayoutItem>& items);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;   // Gain knob follows state loads / undo; RVB/PLX + RTRG/CHO slots follow their switches
    void applyTheme();

    juce::SharedResourcePointer<ThemeManager> theme;
    juce::SharedResourcePointer<LayoutTweaks> layout;   // Edit Layout nudges, applied in resized()
    LfoPanel lfoPanel;                // shelved -- see class comment; not shown, still fully wired
    RetrigPanel retrigPanel;          // 1st slot, shared with chorusPanel: only the selected
    ChorusPanel chorusPanel;          //   one shows (the tab above its pill switches RTRG <-> CHO)
    void syncModSlot();
    MimeophonPanel mimeoPanel;        // 2nd slot -- DLY
    PlexiphonPanel plexPanel;         // 3rd slot, shared with reverbPanel: only the selected
    ReverbPanel reverbPanel;          //   model's panel shows (its pill switches RVB <-> PLX)
    void syncSpaceSlot();

    // Gain -- the output volume, last in the whole chain (after every effect, tails included;
    // see PluginProcessor::processBlock). Standalone only, same as before; moved here from the
    // end of KnobRow to make room for Dirt. Styled like KnobRow's knobs.
    AudioDocument& document;
    const bool showGain;
    juce::Range<int> gainColumn;   // empty until PluginEditor passes it
    int firstEdge = -1;            // Pitch disc's left edge, -1 until PluginEditor passes it
    R3WRKLookAndFeel gainLnF;
    juce::Label gainCaption;
    juce::Slider gainKnob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
