#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoPanel.h"
#include "ReverbPanel.h"
#include "PlexiphonPanel.h"
#include "MimeophonPanel.h"
#include "Theme.h"

/**
    The collapsible "FX drawer" beneath the main KnobRow, revealed by the chevron on its right
    edge (see PluginEditor::toggleFxDrawer(), which grows the window by the drawer's height
    rather than displacing anything already on screen).

    One row: Delay | Plexiphon, left to right (Delay is Mimeophon -- see below). LFO and Reverb
    are SHELVED, not deleted -- their panels (lfoPanel/reverbPanel below) are still real, fully
    wired members (PluginProcessor::tickLfos()/applyReverb() still run exactly as before;
    AudioDocument's lfoSlots/reverbXxx fields still persist), just not addAndMakeVisible()'d or
    given a layout slot here, so they're a two-line change to bring back (see the "Add Erbe-Verb
    reverb, Plexiphon, and a hardware-LCD popup look" commit / its checkpoint tag for the last
    state with both visible). The user preferred Plexiphon over Erbe-Verb and wanted a simpler
    drawer for now.

    Plexiphon and Mimeophon are both real (PluginProcessor::applyPlexiphon()/applyMimeophon())
    -- each owns its whole cell. Mimeophon replaced the old placeholder DELAY Section (fake
    TIME/FDBK knobs, never wired to AudioDocument) -- fittingly, since Mimeophon literally is a
    delay -- so the generic placeholder-Section machinery that used to back it is gone too.
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

    juce::SharedResourcePointer<ThemeManager> theme;
    LfoPanel lfoPanel;                // shelved -- see class comment; not shown, still fully wired
    ReverbPanel reverbPanel;          // shelved -- see class comment; not shown, still fully wired
    MimeophonPanel mimeoPanel;        // 1st slot -- real (replaced the old DELAY placeholder)
    PlexiphonPanel plexPanel;         // 2nd (last) slot -- real

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRow)
};
