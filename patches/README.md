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
