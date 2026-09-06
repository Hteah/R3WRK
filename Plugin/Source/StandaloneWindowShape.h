#pragma once
#include <JuceHeader.h>

// Native-styles the Standalone app's own window (irrelevant to VST3/AU, which are hosted
// inside a DAW's own window and can't reshape it). The patch has already called
// setUsingNativeTitleBar(true) so the close/minimise controls are the real macOS traffic
// lights on the LEFT; this then:
//   - hides the native title-bar strip (titlebarAppearsTransparent + NSWindowTitleHidden +
//     NSWindowStyleMaskFullSizeContentView, movableByWindowBackground) so the app's own dark
//     UI runs the full window height with the buttons just floating over the top-left;
//   - clips the content view to a rounded-rect CALayer mask (so everything JUCE paints, at
//     any nesting depth, gets cropped to the shape -- clipping in a single Component's own
//     paint() wouldn't carry through to child components) over a non-opaque, clear-background
//     NSWindow, so the cropped corners read as genuinely transparent rather than a mismatched
//     solid colour peeking out. PluginEditor keeps a thin top band clear of its own controls
//     in the Standalone build so the floating buttons don't overlap anything.
// JUCE exposes no hook to reach the native NSWindow/CALayer from a Component, hence the .mm.
//
// Called once, right after the window's native peer exists (see the R3WRK patch to
// StandaloneFilterWindow's constructor, patches/juce-standalone-window.patch) --
// cornerRadius is a CALayer property that tracks the layer's current bounds on its own, so
// this doesn't need reapplying on resize.
// extern "C" + void* rather than a juce::Component& -- the JUCE patch that declares/calls this
// (patches/juce-standalone-window.patch) sits inside an ambient, unclosed nested
// `namespace juce { ... }` opened by an earlier JUCE amalgamation header, where *no* spelling
// of the juce:: qualifier (not even the root-relative ::juce::) resolves; void* sidesteps
// needing the type to resolve there at all. Cast back to juce::Component* here instead, where
// there's no such ambient nesting.
#if JUCE_MAC
extern "C" void r3wrkApplyRoundedWindowCorners (void* window, float cornerRadiusPx);
#endif
