# JUCE patches

Small local edits to the vendored JUCE checkout (`../JUCE/`, git-ignored, re-cloned fresh by
`build.sh` or the manual steps in `BUILD_ON_MACOS.md`) that don't have a supported JUCE
customization hook to reach the same result. `build.sh` applies every `.patch` file here
automatically right after a fresh clone; a manual clone needs the same step by hand:

```sh
cd JUCE
git apply ../patches/<name>.patch
```

Each patch is a plain `git diff` taken from inside the `JUCE/` checkout.

## `juce-standalone-window.patch`

All of R3WRK's tweaks to the Standalone app's own window
(`juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h`). None of it affects the
VST3/AU — those are hosted inside a DAW's own window. In one file:

1. **Native title bar.** `StandaloneFilterWindow`'s constructor now calls
   `setUsingNativeTitleBar (true)` (mac only), so the close / minimise controls are the real
   macOS traffic lights on the **left**, like every other Mac app. JUCE's stock Standalone
   wrapper draws its own title bar with the buttons on the right.

2. **Rounded corners + hidden title-bar strip.** Right after the peer exists the constructor
   calls `r3wrkApplyRoundedWindowCorners (this, 12.0f)` (forward-declared at the top of the
   file — not `#include`d, so the patch stays self-contained and doesn't make JUCE code depend
   on R3WRK headers). The work is in `Plugin/Source/StandaloneWindowShape.mm` (mac only): hides
   the title-bar strip (`titlebarAppearsTransparent` + `NSWindowTitleHidden` +
   `NSWindowStyleMaskFullSizeContentView`, `movableByWindowBackground`) so the app's own dark
   UI runs the full height under the floating buttons, and clips the content view to a
   rounded-rect `CALayer` mask with a non-opaque clear-background `NSWindow` behind it so the
   cropped corners read as genuinely transparent. `PluginEditor` keeps a thin band at the top
   clear of its own controls in the Standalone build (a standalone-only top inset).

3. **"Options" button repositioned.** With a native title bar `getTitleBarHeight()` is 0, so
   `resized()` floats the audio-device `optionsButton` at the top-**right**, opposite the
   traffic lights.

4. **Notification banner recoloured + inset.** The built-in "Audio input is muted to avoid
   feedback loop" banner (`NotificationArea`) is recoloured from JUCE's stock bright yellow to
   R3WRK's Midnight-theme colours (hardcoded — this window is outside the plugin editor and
   has no reach into `ThemeManager`), and its text is left-padded on mac so the floating
   traffic lights don't sit on top of it. The feedback-loop detection/muting itself is
   untouched.

If JUCE is ever upgraded to a newer tag, re-check this still applies — `git apply --check`
reports without touching anything.
