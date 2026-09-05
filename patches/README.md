# JUCE patches

Small local edits to the vendored JUCE checkout (`../JUCE/`, git-ignored, re-cloned fresh by
`build.sh` or the manual steps in `BUILD_ON_MACOS.md`) that don't have a supported JUCE
customization hook to reach the same result. `build.sh` applies every `.patch` file here
automatically right after a fresh clone; a manual clone needs the same step by hand (see below).

Each patch is a plain `git diff` taken from inside the `JUCE/` checkout, so it applies with:

```sh
cd JUCE
git apply ../patches/<name>.patch
```

## `juce-standalone-notification-bar.patch`

Recolours the Standalone app's built-in "Audio input is muted to avoid feedback loop" banner
(`juce_StandaloneFilterWindow.h`'s `NotificationArea`) from JUCE's stock bright yellow to
R3WRK's own Midnight-theme colours, so it doesn't clash with the rest of the dark UI. Hardcoded
colours, not read live from `ThemeManager` -- this window lives outside the plugin editor
entirely (it wraps the editor, rather than being part of it), so it has no reach into the app's
theme system without much deeper wiring than a cosmetic fix like this warrants. The feedback-loop
detection and muting itself is untouched, only the banner's appearance.

If JUCE ever gets upgraded to a newer tag, re-check that this still applies cleanly (JUCE's own
`NotificationArea` implementation could change) -- `git apply --check` will say so without
touching anything.

## `juce-standalone-rounded-corners.patch`

Rounds the corners of the Standalone app's own window (irrelevant to VST3/AU -- those are hosted
inside a DAW's own window and can't reshape it). JUCE has no cross-platform "rounded window"
feature and no exposed hook to reach the native NSWindow/CALayer from a `Component`, so this
patch is a single call into R3WRK's own code: `StandaloneFilterWindow`'s constructor gets one
call to `r3wrkApplyRoundedWindowCorners(*this, 12.0f)` right after the window's native peer
exists, forward-declared at the top of the file (not `#include`d, so this patch stays a
self-contained one-line addition to JUCE's own file rather than a new dependency of JUCE code
on R3WRK's own headers). The actual work happens in `Plugin/Source/StandaloneWindowShape.mm`
(mac-only, `#if JUCE_MAC`): clips the window's content view to a rounded-rect `CALayer` mask
(so everything JUCE paints, at any nesting depth, gets cropped to the shape -- clipping inside
a single `Component::paint()` wouldn't reach child components, since JUCE's own paint-scoping
restores the clip before painting children) and makes the underlying `NSWindow` non-opaque with
a clear background, so the four corners the mask crops away read as genuinely transparent (the
desktop showing through) instead of a mismatched solid colour peeking out from behind the
rounded content.

Applies independently of the notification-bar patch above (touches a different, non-overlapping
part of the same file), so the two can be applied in either order.
