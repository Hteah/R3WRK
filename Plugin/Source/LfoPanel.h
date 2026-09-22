#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "LfoModule.h"
#include "Theme.h"

/**
    The LFO slot of the FX drawer: a compact list (one row per visible r3wrk::LfoModule slot on
    AudioDocument -- enable pill + name) plus "+ Add LFO". Each row's list of controls (Shape,
    Range, Rate, Sync, Target, Amount) is too much to fit in a drawer cell, so clicking a row
    opens a popup editor instead -- the same juce::CallOutBox pattern EditorToolbar already uses
    for its own popups (AutoRecordThresholdPanel, BlackBoxDurationPanel, ...).

    Deferred to a later pass (see the LFO design conversation): tempo sync to host BPM (the
    Sync toggle is present but visually inert), and right-click-to-draw a custom shape.
*/
class LfoPanel : public juce::Component,
                 private juce::Timer,
                 private juce::ChangeListener
{
public:
    LfoPanel(AudioDocument& document, bool standalone);
    ~LfoPanel() override;

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;   // low-rate repaint so an external state load's enable/count is reflected
    void changeListenerCallback(juce::ChangeBroadcaster*) override { applyTheme(); }
    void applyTheme();
    void openEditorFor(int slotIndex, juce::Component& anchor);

    struct Row : juce::Component
    {
        LfoPanel& owner;
        int index = 0;
        juce::Colour fill, ink, border, textDim;

        explicit Row(LfoPanel& o, int i) : owner(o), index(i) {}
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseEnter(const juce::MouseEvent&) override { hovered = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
        bool hovered = false;

        static constexpr int kPillW = 20;
    };

    AudioDocument& document;
    bool standaloneBuild;
    juce::SharedResourcePointer<ThemeManager> theme;

    juce::OwnedArray<Row> rows;
    juce::TextButton addButton { "+ ADD LFO" };
    void rebuildRows();   // called once numVisibleLfos changes (add button, or a state load)
    int lastBuiltCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LfoPanel)
};
