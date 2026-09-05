#pragma once
#include <JuceHeader.h>

// Rounds the corners of the Standalone app's own native window (Finder/Dock's "desktop app"
// window -- irrelevant to VST3/AU, which are hosted inside a DAW's own window and can't
// reshape it). JUCE has no cross-platform "rounded window" feature and no exposed hook to
// reach the native NSWindow/CALayer from a Component, so this is genuinely native: clips the
// window's content view to a rounded-rect layer mask (so everything JUCE paints, at any
// nesting depth, gets cropped to the shape -- unlike clipping in a single Component's own
// paint(), which JUCE's own paint-scoping wouldn't carry through to child components) and
// makes the underlying NSWindow itself non-opaque with a clear background, so the four
// corners the mask crops away read as genuinely transparent (the desktop showing through)
// rather than a mismatched solid colour peeking out from behind the rounded content.
//
// Called once, right after the window's native peer exists (see the R3WRK patch to
// StandaloneFilterWindow's constructor, patches/juce-standalone-rounded-corners.patch) --
// cornerRadius is a CALayer property that tracks the layer's current bounds on its own, so
// this doesn't need reapplying on resize.
// extern "C" + void* rather than a juce::Component& -- the JUCE patch that declares/calls this
// (patches/juce-standalone-rounded-corners.patch) sits inside an ambient, unclosed nested
// `namespace juce { ... }` opened by an earlier JUCE amalgamation header, where *no* spelling
// of the juce:: qualifier (not even the root-relative ::juce::) resolves; void* sidesteps
// needing the type to resolve there at all. Cast back to juce::Component* here instead, where
// there's no such ambient nesting.
#if JUCE_MAC
extern "C" void r3wrkApplyRoundedWindowCorners (void* window, float cornerRadiusPx);
#endif
