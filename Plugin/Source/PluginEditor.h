#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "HeaderBar.h"
#include "EditorToolbar.h"
#include "StandaloneMenuBar.h"
#include "KnobRow.h"
#include "FxRow.h"
#include "WaveformDisplay.h"
#include "SpectrogramDisplay.h"
#include "TimeRuler.h"
#include "Theme.h"
#include "OutputSettings.h"
#include "R3WRKLookAndFeel.h"

class R3WRKAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::ChangeListener,
                                  public juce::FileDragAndDropTarget
{
public:
    explicit R3WRKAudioProcessorEditor(R3WRKAudioProcessor&);
    ~R3WRKAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    // Standalone/macOS: the reserved top inset stands in for a hidden title bar -- drag it to
    // move the window, double-click it to zoom (fill the screen). The move only begins on
    // mouseDrag so a plain double-click still reaches mouseDoubleClick.
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void parentHierarchyChanged() override;   // apply the persisted float-on-top once the peer exists

    // Drag a sample in from Finder (or a DAW's browser) and drop it anywhere on the window to
    // load it, same as Tools ▾ -> "Open…" -- covers the whole editor rather than just the
    // waveform, since none of the child components (WaveformDisplay included) implement
    // FileDragAndDropTarget themselves, so JUCE's peer keeps walking up the component
    // hierarchy from whatever's under the cursor until it reaches this one.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void applyHeaderButtonThemes();   // themes the follow + float-on-top header buttons
    void applyFloatOnTop(bool on);   // toggles the native window level + persists
    void maybeApplyPersistedFloatOnTop();   // once, as soon as the window peer exists
    void toggleFxDrawer();   // KnobRow's chevron -- grows/shrinks the window by one row

    bool showingDropHighlight = false;

    // FX drawer: closed by default, so the plugin's footprint is unchanged until the user asks
    // for it. Not persisted yet -- reopens closed every time the editor is recreated.
    // One row (see FxRow's class comment) -- the same strip height as KnobRow. Used to be two
    // stacked rows, but LFO and Reverb moved their real controls behind a popup editor rather
    // than needing inline space, so all four effects fit across one row now.
    static constexpr int kFxRowHeight = 83;
    static constexpr int kFxRowGap    = 4;
    bool fxDrawerOpen = false;

    // In the Standalone build the macOS traffic lights float over the top-left of our own UI
    // (native title bar, no strip -- see StandaloneWindowShape.mm), so reserve a thin band at
    // the top for them. Zero in a plugin (the DAW owns the window chrome).
    static constexpr int kMacTrafficLightInset = 22;
    const bool standaloneWindow;

    // A press in the top inset arms a window move; it only actually starts once the mouse
    // moves (mouseDrag), so AppKit's nested drag loop never runs for a plain double-click and
    // that click reaches mouseDoubleClick -> zoom, like a native title bar.
    bool windowDragArmed = false;
    bool windowDragActive = false;

    R3WRKAudioProcessor& processorRef;
    juce::SharedResourcePointer<ThemeManager> theme;
    juce::SharedResourcePointer<OutputSettings> outputSettings;

    // Header-row corner buttons, boxless (no background at all, on or off -- per the user's
    // request to remove the brace/box around them, same treatment KnobRow's drawer toggle
    // already got) sharing one look-and-feel:
    //   followButton     -- Follow-playhead toggle (drives document.followPlayheadEnabled;
    //                        WaveformDisplay's 30 Hz timer does the actual view-following).
    //                        Every build. Moved here from the transport strip.
    //   floatOnTopButton -- keep the window above other apps. Standalone only. Native
    //                        NSWindow level, persisted via OutputSettings.
    // EXPERIMENT (branch experiment/space-mono-font): JUCE resolves every Font's typeface
    // through the process-wide juce::LookAndFeel::getDefaultLookAndFeel() singleton
    // (juce_LookAndFeel.cpp's getTypefaceForFontFromLookAndFeel), not through whichever
    // LookAndFeel is attached to the component doing the drawing -- so
    // R3WRKLookAndFeel::getTypefaceForFont only has any effect once an instance is
    // installed as that default. Every other LookAndFeel member below (cornerButtonLnF,
    // toolbarLnF, knobLnF, ...) already inherits the same override, so this one instance
    // covers plain Labels too (HeaderBar, KnobRow captions, ...) that never get an
    // explicit setLookAndFeel() of their own.
    R3WRKLookAndFeel fontLnf;
    R3WRKIconOnlyLookAndFeel cornerButtonLnF;
    juce::TextButton followButton    { R3WRKLookAndFeel::iconFollow };
    juce::TextButton floatOnTopButton { R3WRKLookAndFeel::iconFloatTop };
    bool floatStateApplied = false;

    HeaderBar header;
    WaveformDisplay waveformDisplay;
    SpectrogramDisplay spectrogramDisplay;
    TimeRuler timeRuler;
    EditorToolbar toolbar;
    KnobRow knobRow;
    FxRow fxRow;

    // Standalone only: the macOS application menu bar (File / Edit / Tools). nullptr in a
    // plugin (the host owns the menu bar). Installed via setMacMainMenu() in the ctor,
    // cleared in the dtor.
    std::unique_ptr<StandaloneMenuBar> macMenuBar;

    // Plugins shouldn't open native windows of their own, so this is given `this` as its
    // parentComponent (per the class's own docs) rather than defaulting to the desktop --
    // it then stays invisible until the mouse hovers a component with a tooltip set, and
    // scales with the editor/DAW like any other child. Without this, setTooltip() calls
    // anywhere in the editor (e.g. EditorToolbar's icon buttons) are wired but silent.
    juce::TooltipWindow tooltipWindow { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(R3WRKAudioProcessorEditor)
};
