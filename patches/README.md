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

3. **"Options" button hidden + a bridge to the audio settings.** With a native title bar the
   editor content fills the whole window, so JUCE's `optionsButton` (added before
   `setContentOwned`) rendered behind it and was unclickable anyway. It's now
   `setVisible(false)` on mac; the Audio/MIDI settings are reached from R3WRK's own menu bar /
   Tools menu ("Audio Settings…"). Since `StandalonePluginHolder` is only visible inside
   `juce_audio_plugin_client_Standalone.cpp`, the patch adds an `extern "C" void
   r3wrkShowAudioSettings()` at the end of the header (calls
   `StandalonePluginHolder::getInstance()->showAudioSettingsDialog()`); `EditorToolbar` calls
   it, and `StandaloneWindowShape.mm` carries a `__attribute__((weak))` no-op of the same name
   so the VST3/AU targets (which don't compile that TU) still link — the strong definition
   wins in the Standalone target.

4. **Notification banner suppressed (mac) + recoloured.** `MainContentComponent::inputMutedChanged`
   forces `newInputMutedValue = false` on mac, so the built-in "Audio input is muted to avoid
   feedback loop" banner (`NotificationArea` — the black strip at the top of the window) never
   shows and the window never grows to make room for it. The muting itself (`muteInput`, and
   the "Mute audio input" checkbox in Audio Settings) is untouched; the checkbox is now the
   only place that state is surfaced. The banner is still recoloured from JUCE's stock bright
   yellow to R3WRK's Midnight-theme colours (hardcoded — this window is outside the plugin
   editor and has no reach into `ThemeManager`) for the non-mac builds, and its text is
   left-padded on mac so the floating
   traffic lights don't sit on top of it. The feedback-loop detection/muting itself is
   untouched.

If JUCE is ever upgraded to a newer tag, re-check this still applies — `git apply --check`
reports without touching anything.
