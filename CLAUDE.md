# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

R3WRK is an Edison-style pop-out audio recorder/editor, built as a JUCE plugin (VST3 + AU +
Standalone app): waveform + spectrogram views, record/play/loop, cut/copy/paste/trim/delete/undo,
normalize/gain/fade/reverse/silence, lofi time/pitch effects (tape Speed, Paulstretch Stretch, granular+grit Pitch), export-selection
to WAV, a live knob row (tape Speed/Pitch/Stretch, a Monomachine-modelled multimode filter,
Start/End selection), and an FX drawer (a Monomachine-modelled CHORUS / RTRG buffer retrig sharing one slot, Mimeophon delay, Plexiphon reverb-FDN, Erbe-Verb reverb;
LFO built but currently shelved/hidden). Builds and runs on macOS only (Apple Silicon, Xcode, JUCE
8.0.15).

## Build / test / install

```bash
./build.sh Release          # clones JUCE + applies patches/ if missing, builds everything, installs VST3/AU
```

Equivalent manual steps (see `BUILD_ON_MACOS.md` for full prerequisites):

```bash
cd Plugin
cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .
cmake --build build --config Release --target R3WRK_Standalone R3WRK_VST3 R3WRK_AU
```

`COPY_PLUGIN_AFTER_BUILD TRUE` in `Plugin/CMakeLists.txt` means every build already installs
VST3/AU into `~/Library/Audio/Plug-Ins/`. The Standalone `.app` is **not** auto-relaunched — an
already-running instance must be killed and reopened to pick up a new build:

```bash
pkill -x R3WRK 2>/dev/null; sleep 1; open Plugin/build/R3WRK_artefacts/Release/Standalone/R3WRK.app
```

Headless engine test (no GUI/audio device — safe anywhere):

```bash
cd Plugin
cmake --build build --config Release --target R3WRKSmokeTest
./build/R3WRKSmokeTest_artefacts/Release/R3WRKSmokeTest
```

`Tests/SmokeTest.cpp` is the single test binary (no per-test filtering flag — it runs everything
in one pass). It covers `AudioDocument`/`EditActions`/`TimeStretchEngine` correctness plus DSP
verification for the FX engines (Erbe-Verb, Plexiphon, Mimeophon, the Monomachine filter model,
LFO). **New DSP always gets an offline SmokeTest assertion before going live** — this has caught
real bugs for free more than once (e.g. an `inf` under stress in the old direct-form biquad; see
`patches/README.md`-adjacent history in `PROJECT_NOTES.md` for the pattern).

`JUCE/` (git-ignored, cloned by `build.sh`) must sit next to `Plugin/` — `CMakeLists.txt` does
`add_subdirectory(../JUCE)`. `patches/*.patch` are small local edits to the vendored JUCE checkout
itself (see `patches/README.md`); `build.sh` applies them automatically after a fresh clone.

No third-party DSP libraries beyond JUCE: time/pitch is R3WRK's own `Source/LofiStretch.h`
(Rubber Band, GPL, was removed on 2026-09-27 so R3WRK can be sold / closed-source with only a
JUCE licence — don't reintroduce GPL code).

### Signing

Ad-hoc signed by default (zero setup, but macOS ties the Screen Recording (TCC) grant to the exact
signature hash, so it re-changes — and needs re-approving — on every rebuild). To sign with a real
Apple identity instead (stable across rebuilds), copy `Plugin/Signing.local.cmake.example` to
`Plugin/Signing.local.cmake` (git-ignored) and fill in your identity/team.

## Architecture

### The document/edit/undo model

`AudioDocument` (`Source/AudioDocument.h/.cpp`) is the single source of truth: the sample buffer,
selection, loop points, playhead, and every live playback-knob parameter (`std::atomic` fields, not
processor-owned — so both the audio thread and GUI views can read them). Edits are **snapshot-based**,
not diff-based: `beginChange()` copies the buffer, the caller builds a new one via `EditActions`
helpers (`Source/EditActions.h/.cpp` — cut/copy/paste/trim/delete/normalize/gain/fade/reverse/
silence/export-selection), and `commitChange(newBuffer, name)` pushes a `juce::UndoableAction` that
swaps between the two full copies. Deliberate v1 trade-off: easy to reason about, memory scales with
(audio length) × (undo steps kept), capped by `UndoManager::setMaxNumberOfStoredUnits`.

### Thread safety

The GUI thread can resize/replace the buffer at any time (edits, undo/redo, file loads); the audio
thread reads it during playback. Every live-buffer mutation funnels through
`AudioDocument::restoreSnapshot()`, which takes a `juce::CriticalSection`; `processBlock()` takes the
same lock via a **try-lock** (never blocks indefinitely). Plain critical-section design, not a
lock-free double-buffer — fine for an editing tool, not for an ultra-low-latency insert. The
selection is packed into one `std::atomic<uint64_t>` (`selPacked`) rather than two atomics, so
`processBlock` never reads a torn start/end pair mid-drag.

### Playback chain (`PluginProcessor::processBlock`)

Plays the document region (selection, else loop points, else whole clip) from the buffer under a
try-lock (loop reading: `Source/RegionGather.h`), then runs a fixed chain:
**NaN safety net → Dirt → filter → Chorus → RTRG → Mimeophon → Reverb/Plexiphon → Gain (master volume,
last) → NaN safety net → capture-output**. Notes on the newer pieces:
- **Dirt** (`DirtStage.h`, popup `DirtPanel.h`): Octatrack-style drive → sample-rate → bits, ahead
  of the filter. Defaults are a bit-exact bypass.
- **Chorus** (`MnmChorusEngine.h`, panel `ChorusPanel.h`, doc `MNM_CHORUS_PLAN.md` at the repo
  root with the measured FINDINGS as an appendix): a clone of the Monomachine FX-CHORUS machine,
  eight params DEL DEP SPD MIX / FB WID LP INP (raw 0..127, drawer shows DEL + MIX). Every
  constant is in `MonomachineChorusModel.h`, GENERATED by the companion project
  `~/Documents/Claude/MNMCHORUS` (`fit.py export`, black-box measurements of the MnM OS in the
  AGPL Monomodule emulator -- only numbers cross over, never its code). Golden vectors
  (`Tests/MnmChorusGolden.h`, same generator) make the smoke test null the engine against the
  emulator. Shares the first drawer slot with RTRG (`AudioDocument::fxSlotChorus`, switched by the
  small `SlotSwitchTab` above each pill); only the shown one runs. Resets only on its enable edge;
  has an idle tail after Stop (FB near 127 rings almost forever, like the hardware).
- **Gain** is the last stage (effect tails included, also on the idle tail path); its knob lives at
  the end of the FX drawer (Standalone only).
- **Safety net** (`AudioSafety.h::zeroNonFinite`): before Dirt and before the output. Dirt, the
  filter and the FX effects all have feedback state -- one NaN used to silence a channel until
  restart. Keep both calls when touching the chain.
- **Drag-scan** (dragging a loop edge/knob while looping): `DragScanRender.h` -- constant 1x read,
  per-sample edges, relocation crossfades, release tail, window speed capped under the read speed
  (`maxWindowSpeed`). With Speed/Pitch/Stretch engaged it feeds the stretcher's input instead.
- **Overdub** (`OverdubWriter.h`, popup `OverdubPanel.h`): writes input into the playing loop,
  `doc = doc*Feedback + in*Level`, timed via gatherRegion's per-sample position trace and the round-
  trip latency (Standalone: `r3wrkDeviceRoundTripLatency()` hook in the patched Standalone window;
  0 in VST/AU). One pass = one undo step (`AudioDocument::begin/endOverdubPass`). Disabled while
  Speed/Stretch/Pitch are engaged.
- **Monitor** (toolbar headphones): hear the input over the loop / while stopped without
  recording; DRY (after Gain) or FX (mixed in before Dirt). Never restored ON from state.
- **Plexiphon v2** (`PlexiphonEngine.h`): two 8-line FDNs (L/R), PLEXUS = active lines + echo→reverb
  line lengths + identity→Hadamard; COUPLE/SKEW stereo; cut-only COLOR.
- **RVB / PLX slot** (`applySpaceSlot()`): Erbe-Verb and Plexiphon share the last drawer slot.
  The `SlotSwitchTab` above the pill picks the model (`reverbEnabled` / `plexEnabled`); the pill is
  the slot's on/off (`AudioDocument::spaceOn`, state R3WW; older projects load it on). Switching
  off or switching models lets the tail ring out instead of cutting it. MIX 0 is just silent.
Speed/Pitch/Stretch atomics on `AudioDocument` decide the playback engine: when all three are
centred (1/0/1) it's a plain sample copy (zero latency); otherwise it routes through `r3wrk::LofiStretch` (`Source/LofiStretch.h`, built per
`prepareToPlay`): Speed = tape varispeed (time + pitch together), Stretch = Paulstretch (always
smeared — an effect, not a clean stretch), Pitch = granular shifter + 14.7 kHz/12-bit grit. The
same engine does the offline bake (`TimeStretchEngine`), so exports sound like playback. The stored audio is never modified by these — they're
non-destructive, baked into Save/Export only via `renderWithPlaybackKnobs`. The **scrub** path
(dragging the playhead) duplicates this same filter/FX sequence rather than sharing code with the
linear-playback branch, a deliberate small-duplication choice to avoid touching that
already-proven, delicate branch.

### The multimode filter (Base/Width/HP Q/LP Q)

Modelled from real measurements of the Elektron Monomachine's filter (companion project
`~/Documents/Claude/MNMFILTER`, Python — `model.py export` regenerates
`Source/MonomachineFilterModel.h`). `Source/BiquadFilter.h` is header-only, deliberately **not**
`juce::dsp` (so identical math runs both realtime in the plugin and offline in the smoke test, which
has no `juce_dsp` dependency) — `r3wrk::MultiModeFilter` chains one HP + one LP 2-pole stage, each
self-bypassing when open.

### FX drawer (Mimeophon / Plexiphon / Reverb / LFO)

`FxRow` (`Source/FxRow.h/.cpp`) is a collapsible row under the main `KnobRow`, toggled by
`PluginEditor::toggleFxDrawer()` (grows the window rather than displacing anything). Each effect is
a real DSP engine, header-only, `namespace r3wrk`, plain doubles/no allocation after `prepare()` —
**not** a class hierarchy: one plain struct per effect (no virtual dispatch), matching this
codebase's established style:

- `Source/ReverbEngine.h` — 4-line FDN reverb modelled on the Make Noise Erbe-Verb. Also home to
  shared FDN-family primitives (`DelayLine`, `AllpassDiffuser`, `OnePoleLowpass`, `Biquad` shelf
  helpers, `chebyshevPerturb`) that `MimeophonEngine.h`/`PlexiphonEngine.h` both `#include` and
  reuse rather than duplicating.
- `Source/PlexiphonEngine.h` — 8-line FDN with a continuously-morphing feedback matrix (sparse
  permutation ↔ dense Hadamard), Make Noise Plexiphon.
- `Source/MimeophonEngine.h` — stereo tape/BBD-style echo, Make Noise/Tom Erbe Mimeophon (Zone,
  Rate, Repeats, Color, Halo, Mix, Skew, Ping-Pong).
- `Source/MnmChorusEngine.h` + generated `Source/MonomachineChorusModel.h` — Monomachine FX-CHORUS
  clone (see MNM_CHORUS_PLAN.md; regenerate the model from MNMCHORUS, don't hand-edit). Its drawer
  cell `ChorusPanel.h` shares RTRG's slot (`RetrigEngine.h`/`RetrigPanel.h`).
- `Source/LfoModule.h` + `Source/LfoPanel.h/.cpp` — free-running LFO (5 shapes, banded rate),
  ticked once per block (not per sample). **Currently shelved**: fully wired
  (`PluginProcessor::tickLfos()`, `AudioDocument`'s `lfoSlots` persist), but not
  `addAndMakeVisible()`'d in `FxRow` and forced to zero active slots via
  `PluginProcessor::numActiveLfoSlots()`'s `kLfoFeatureShelved` switch — re-enabling is a two-line
  change (see `FxRow.h`'s own comment), not a rewrite.

Each engine has a matching compact `*Panel` (`MimeophonPanel`, `PlexiphonPanel`, `ReverbPanel`) that
lives directly in the drawer (2 primary knobs + enable pill), plus a `juce::CallOutBox` popup
(launched from a "…" button) exposing the full parameter set. `PluginProcessor::applyMimeophon()` /
`applyPlexiphon()` / `applyReverb()` apply them in the fixed chain order above.

**Research docs**: each engine's own header comment cites the external research doc it was built
from (`~/Downloads/erbe-verb-ARCHITECTURE.md`, `PLEXIPHON_PLAN.md`, `MIMEOPHON_PLAN.md`) — read
those first before extending an engine; they carry the "why this constant" reasoning that doesn't
belong duplicated in code comments.

### Custom UI (`R3WRKLookAndFeel`)

`Source/R3WRKLookAndFeel.h/.cpp` is the shared `LookAndFeel_V4` subclass: flat-disc rotary knobs (no
value-arc), pill-shaped buttons, and hand-drawn vector icons (`iconPlay`, `iconLoop`, `iconOrbit`,
...) selected by matching `TextButton::getButtonText()` against `icon:...` marker constants rather
than drawing real text — see `drawButtonText()`. `R3WRKIconOnlyLookAndFeel` is a zero-background
variant (no box/pill at all, ever) for boxless toggle icons; toggle state reads purely through ink
colour (`textColourOnId` vs `textColourOffId`), never a filled background, for anything using it.

One instance (`fontLnf` in `PluginEditor`) is installed as the **app-wide default**
(`juce::LookAndFeel::setDefaultLookAndFeel`) so `getTypefaceForFont()` can swap every font for the
embedded Space Mono typeface without touching individual `setFont()` call sites. **Gotcha**: this
happens in the `PluginEditor` constructor *body*, which runs after all member components are already
constructed — any slider/label built as a `PluginEditor` member (or a member of one of its child
components, e.g. the FX drawer panels) that never gets an explicit `setLookAndFeel()` call of its
own will have already cached its font/textbox against whatever the *true* JUCE default was at
construction time, not `fontLnf`. The fix, used throughout (`KnobRow`'s knobs, each FX panel's
knobs), is to give every such component its own small `R3WRKLookAndFeel` member and explicitly
`setLookAndFeel(&that)`/`setLookAndFeel(nullptr)` (attach in the ctor, detach before the member is
destroyed in the dtor) — don't rely on the global default alone for anything constructed as part of
the editor's own member list.

`spaceMonoFont(height, bold=false)` / `systemUIFont(height, bold=false)` (free functions, declared
in `R3WRKLookAndFeel.h`) attach an already-resolved typeface directly to a `Font`, bypassing the
`getTypefaceForFont()` override — used where a specific typeface (not just "whatever's app-default")
must be guaranteed, or where Space Mono's monospace strokes read poorly at very small sizes (e.g.
`systemUIFont` for tiny badges/pills).

### State persistence

`PluginProcessor::getStateInformation()`/`setStateInformation()` use an incrementing 4-byte magic
constant (`kStateMagic`, e.g. `'R3WN'` → `'R3WO'`) per format version. Each new persisted field adds
one more letter and a cascading `bool hasX = (hasY || magic == kStateMagicRZWx)` chain gating an
extra `readDouble()`/`readBool()` block, with a safe default for older blobs — never break backward
compatibility with a saved project by reordering or removing an existing read.

### Theming

`Source/Theme.h/.cpp` — `Palette` (15 editable colours, shared-theme text format with Sieve) + `ThemeManager`, a process-wide
`juce::SharedResourcePointer` singleton persisted to its own settings file. Components read it via
their own `SharedResourcePointer<ThemeManager>` member and re-theme on `ChangeListener` callbacks —
not injected top-down.

## Conventions worth knowing before extending this codebase

- **No class hierarchies for DSP.** One plain `namespace r3wrk` struct per effect, `noexcept`,
  plain doubles, no allocation after `prepare()`/construction, no virtual dispatch. This applies to
  new effects too — don't introduce a base class.
- **New DSP gets an offline `SmokeTest.cpp` assertion before it's wired live** — this is a project
  norm, not a suggestion; it has caught real numerical bugs (`inf`/instability under stress) that
  weren't audible in casual listening.
- **Prefer small, explicit duplication over a shared refactor of already-proven, delicate code** —
  e.g. the scrub-playback branch duplicates the filter/FX chain rather than being merged with the
  main linear-playback branch, specifically to avoid touching that branch's history of subtle bugs.
- Every `LookAndFeel` attached to a component is an **attach/detach pair**:
  `component.setLookAndFeel(&member)` in the constructor, `component.setLookAndFeel(nullptr)` in the
  destructor *before* the member instance is destroyed.
- `PROJECT_NOTES.md` (git-ignored-adjacent but tracked, ~150KB) is a detailed running design log —
  useful for the *reasoning* behind older subsystems (undo model, visual time-stretch, filter
  history, known gaps), but it predates the FX drawer / Mimeophon / Plexiphon / Erbe-Verb / LFO
  work entirely, so treat it as historical background for the pre-FX-drawer engine, not a current
  map of `Source/`.
