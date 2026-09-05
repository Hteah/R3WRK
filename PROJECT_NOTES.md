# R3WRK — project notes

An Edison-style pop-out audio recorder/editor, built as a JUCE plugin
(VST3 + AU + standalone app): waveform + spectrogram views, record/play/loop,
cut/copy/paste/trim/delete/undo, normalize/gain/fade/reverse/silence,
RubberBand-powered time-stretch & pitch-shift, export-selection to WAV, and a
live knob row (tape Speed, Pitch and extreme Stretch that reshape playback in
real time; Start/End that drive the selection edges).

Builds natively on macOS (VST3 + AU + Standalone); see `BUILD_ON_MACOS.md`.

## Layout

```
R3WRK/
  JUCE/                    JUCE framework (cloned separately, see BUILD_ON_MACOS.md)
  Plugin/
    CMakeLists.txt
    Source/
      AudioDocument.h/.cpp     the document: buffer, selection, playhead, loop,
                                undo history (snapshot-based)
      EditActions.h/.cpp       cut/copy/paste/trim/delete/normalize/gain/
                                fade/reverse/silence/export-selection, all
                                operating on AudioDocument as single undo steps
      TimeStretchEngine.h/.cpp offline time-stretch/pitch-shift via librubberband
      WaveformDisplay.h/.cpp   waveform view: draw, select, zoom, scroll
      SpectrogramDisplay.h/.cpp FFT spectrogram view of the whole document
                                (built, but no UI toggle reaches it right now)
      EditorToolbar.h/.cpp     transport strip + Tools menu; owns the clipboard,
                                talks directly to AudioDocument/EditActions
      KnobRow.h/.cpp           rotary knob strip under the transport bar
                                (Pitch, Speed, Stretch, Start, End; extensible)
      R3WRKLookAndFeel.h/.cpp  shared custom look: rotary knobs (flat disc +
                                pointer line, no value-arc) and pill-shaped
                                buttons (fully rounded; theme-aware)
      TimeRuler.h/.cpp          time ruler under the waveform; follows the
                                waveform's view range, nice tick spacing
      Theme.h/.cpp              Palette (11 editable colours) + ThemeManager
                                (process-wide, persists to a settings file)
      ThemeEditor.h/.cpp        the Tools -> Theme... panel: presets, hex rows,
                                in-panel colour picker, save custom presets
      OutputSettings.h/.cpp     the auto-save output folder (process-wide, own
                                settings file) + timestamped WAV name minting
      PluginProcessor.h/.cpp   audio thread: record / playback (direct or via a
                                real-time RubberBand tape+pitch engine) / pass-
                                through, plugin state save/load
      PluginEditor.h/.cpp      wires HeaderBar + WaveformDisplay + TimeRuler +
                                EditorToolbar + KnobRow together (SpectrogramDisplay
                                is built as a hidden child, currently unreachable)
    Tests/
      SmokeTest.cpp            headless correctness tests for the engine
                                (no GUI/audio device needed) — see below
```

## Running the tests

```
cd Plugin
cmake -B build .
cmake --build build --target R3WRKSmokeTest
./build/R3WRKSmokeTest_artefacts/R3WRKSmokeTest    # (Release/ suffix on multi-config generators)
```

It builds a sine wave in memory and exercises selection, copy/cut/paste,
trim/delete/insert-silence, gain/normalize/fade/reverse/silence, export-selection
to WAV, RubberBand time-stretch/pitch-shift at several ratios, the exact
`replaceRangeWith` path the "Apply Stretch/Pitch" button uses, and a save-to-WAV /
load-from-WAV round trip including resample-on-load.

## How editing/undo works

Edits are snapshot-based rather than diff-based: `AudioDocument::beginChange()`
copies the current buffer, the caller builds a new buffer (via `EditActions`
helpers, which never touch the live buffer directly), and
`AudioDocument::commitChange(newBuffer, name)` pushes a `juce::UndoableAction`
that swaps between the two full copies. This was the deliberate simplicity
trade-off for a v1: it's easy to reason about and hard to get subtly wrong,
at the cost of memory scaling with (audio length) × (undo steps kept). The
undo history is capped by total size (`UndoManager::setMaxNumberOfStoredUnits`),
so it drops the oldest steps rather than growing without bound. For typical
Edison-style use — chopping/editing individual samples/loops rather than
hour-long recordings — that's a fine trade.

## Visual time-stretch

The waveform visually reshapes to match Speed/Pitch/Stretch, like a hardware
sampler: `AudioDocument::getTimeScale()` returns `stretch/speed` (pitch doesn't
affect duration, so it's excluded) — how much longer (>1) or shorter (<1)
playback is than the stored audio. `WaveformDisplay::xToSample()`/`sampleToX()`
(public — `TimeRuler` uses `sampleToX()` for the playhead marker) and
`rebuildWaveformPath()` divide raw samples-per-pixel by this factor, so the same
raw audio is drawn spread across more pixels (visually "zoomed in", looks
longer) or fewer (leaves blank space, looks shorter) than the raw view span
alone would produce. `zoomToward()`/`panByPixels()` convert pointer/drag pixels
through the same timeScale-aware density so zoom/pan stay correct; the
`WaveformDisplay` timer rebuilds the path whenever the knobs move, live, even
while stopped.

This is **purely a drawing thing** — `viewStart`/`viewEnd`, the selection, loop
points, and everything Export/Save write out are still in raw-sample terms,
completely unaffected. The time ruler's tick *labels* don't need any of this:
they're generated as output-time values first, which come out
timescale-invariant for a given raw view window (worked out algebraically —
`x = outputTime · sr · width / rangeLen`, independent of `timeScale`).

## Waveform peak cache

`WaveformDisplay` keeps a peak cache — one min/max per 64 source samples per
channel — rebuilt (`rebuildPeakCache()`) only when the audio content changes.
Zoom / pan / resize call `rebuildWaveformPath()`, which builds the display
`juce::Path` from that cache with **no lock and no `document.getBuffer()`
access**. This matters because `processBlock` guards playback with a try-lock on
`document.getLock()` and emits a silent block when it fails — so the old
"scan the buffer under the lock on every wheel event" made playback click while
zooming in a DAW. Deep zoom (< 64 samples/pixel) copies just the small visible
span out under a brief `ScopedLock` for per-sample detail; `rebuildPeakCache()`
holds the lock only for one `makeCopyOf`.

## Thread safety

The GUI/message thread can resize or replace the document's buffer at any
time (any edit, any undo/redo, loading a file). The audio thread reads that
same buffer during playback. Every point that actually mutates the live
buffer funnels through `AudioDocument::restoreSnapshot()`, which takes a
`juce::CriticalSection`; `processBlock()` takes the same lock (via a
try-lock, so the audio thread never blocks indefinitely) before reading the
buffer for playback. This is a plain-critical-section design, not a
lock-free double-buffer — reasonable for an editing/sampling tool, not
recommended if you extend this into an ultra-low-latency mastering insert.

The selection (start+end) is packed into one `std::atomic<uint64_t>`
(`AudioDocument::selPacked`, unpacked by `getSelection()`) rather than two
separate atomics — with two atomics, `processBlock` could read a new start
paired with a stale end while `setSelection()` was mid-update (e.g. dragging a
selection's start edge, or the KnobRow Start knob), momentarily computing a
tiny/wrong loop region and producing an audible micro-loop buzz for one block.

## Recording model

Recording captures into a separate growable accumulator buffer on the audio
thread (cheap, allocation-free after the first grow) and is only merged into
the document (as a normal undoable "Record" edit) when you stop. Recording
currently always replaces the whole document — there's no overdub/punch-in
yet.

While recording, `processBlock` also feeds a lock-free scope ring on
`AudioDocument` (block peak min/max per 256-sample hop) and a `recordedSamples`
counter. `WaveformDisplay::paint` swaps to a scrolling scope of the incoming
audio + a `REC m:ss.ss` tag while `isRecording`, and the toolbar's time / REC
labels count up from `recordedSamples` — otherwise there's no visual sign a
take is in progress (the playhead only moves during playback).

## Playback & the knob row

`processBlock` plays the document region (the selection if any, else the loop
points, else the whole clip) straight from the buffer under a try-lock. The
`KnobRow` under the transport bar adds:

- **Speed** — tape rate. 1.0 = normal; 2.0 plays twice as fast *and* an octave
  up, pitch riding along exactly like tape.
- **Pitch** — an extra ± semitone shift layered on top of the tape speed, so you
  can detune without changing the playback rate.
- **Stretch** — pure time-stretch, pitch preserved. 1.0 = off; up to 50× for
  "extreme stretch" (smeary by design). Skewed so 1× sits mid-travel.
- **Start / End** — normalised (0–1) knobs on the document selection edges,
  reading out as `m:ss.mmm` and following bracket drags. Start slides the whole
  window (End moves with it, length preserved, pegs at the buffer end); End
  moves independently to change the length.

Speed / Pitch / Stretch live as `std::atomic<double>` on `AudioDocument` (not the
processor — so views can read them too, see "Visual time-stretch" below). When
*all three* are centred (`speed==1`, `pitch==0`, `stretch==1`) playback is a plain
sample copy — zero latency, zero cost. As soon as any is off-centre, playback
routes through a **real-time RubberBand stretcher** (`OptionProcessRealTime |
OptionPitchHighConsistency`), built per `prepareToPlay`:
`timeRatio = stretch / speed`, `pitchScale = speed * 2^(pitch/12)` (so stretch
dilates time without touching pitch, speed is tape). The stretcher is fed
doc-region samples in `getSamplesRequired()`-sized blocks from preallocated
scratch buffers and drained via `available()`/`retrieve()`; it's `reset()` at the
start of each play pass and whenever the knobs cross the bypass/engaged line, and
the `rtFinished` flag makes sure the end-of-region final block is sent exactly
once. The stored audio is never modified — Export Selection still writes the dry
clip. The red playhead tracks the doc read cursor, so it runs slightly ahead of
what you hear by the stretcher latency when the knobs are engaged (more so at
high Stretch).

## Saving / output folder

`Source/OutputSettings.{h,cpp}` is a process-wide `juce::SharedResourcePointer`
singleton with its own `~/Library/Application Support/R3WRK/output.settings`.
It holds the **output folder** (default `~/Music/R3WRK`, created on demand) and
mints timestamped WAV filenames — `R3WRK 2026-09-04 14.22.03.wav` for recordings,
`… selection.wav` for exports, via `getNonexistentSibling()` so same-second saves
don't collide.

- **Stop recording** → `EditorToolbar::autoSaveRecording()` writes the take to the
  folder right away, no dialog; the header name becomes the file and the dirty
  dot clears.
- **Tools ▾ → "Export Selection to Folder"** → the selection is written to the
  folder immediately (was a save dialog).
- **Tools ▾ → "Output Folder…"** → a directory chooser to change it.
- **"Save As…"** is still a dialog (for one-offs / specific names); it now opens
  in the output folder.
- **Drag out:** press inside the selection body (not near an edge) and drag —
  `WaveformDisplay::beginSelectionDragExport()` writes the selection to
  `<temp>/R3WRK/R3WRK selection <timestamp>.wav` and starts a native file drag
  (`DragAndDropContainer::performExternalDragDropOfFiles`, `canMoveFiles=false`).
  Drop it on an Ableton track / in Finder. Temp files >10 min old are swept each
  drag.

Feedback is a `HeaderBar::flashMessage()` — a ~3 s accent-coloured line in the
readout area ("Saved …", "Exported …", "Output folder: …"), driven from
`EditorToolbar` through its `onStatusMessage` callback.

## Custom look (R3WRKLookAndFeel)

One shared `R3WRKLookAndFeel` (per-owner instance, stateless beyond its own
`SharedResourcePointer<ThemeManager>`, so every instance renders identically)
covers two things so far:

- **Rotary knobs** (`drawRotarySlider`) — a flat disc, thin outline, short
  pointer tick near the rim, no value-arc. Used by `KnobRow`.
- **Buttons** (`drawButtonBackground`) — fully rounded pills (radius = half the
  button height). A component asking for a fully transparent `buttonColourId`
  gets the outline treatment (hairline border + faint hover/press wash)
  instead of a solid fill — that's how `EditorToolbar` gets both filled
  (Play/Record, Loop-when-on) and outlined (Tools, Loop-when-off) buttons from
  one draw routine. `loopButton` is a toggling `TextButton`
  (`setClickingTogglesState(true)`), not a `ToggleButton`, so it's part of the
  same pill family instead of rendering as a checkbox.

Any owner must `setLookAndFeel(nullptr)` on every component it attached this
to, in its destructor, *before* its own `R3WRKLookAndFeel` member is destroyed
— both `KnobRow` and `EditorToolbar` do this.

`EditorToolbar::paint()` fills a rounded `panelBg` panel (the dark control
band) behind the whole transport row — the outlined buttons (Loop off, Tools)
and `timeLabel` sit on it, so they use `screenText`/`screenTextDim` rather than
`text`/`textDim`. `R3WRKLookAndFeel`'s outline-button ink comes from whichever
`textColourOffId` the button's owner already set (not read from the theme
directly), so the one shared instance works for buttons in either context
without knowing which it's in.

Not yet covered: `ComboBox`/`PopupMenu` (ThemeEditor's preset picker, the Tools
menu's own popup) still use the default JUCE look, and the waveform/ruler
panels are still square-cornered (left that way deliberately — the waveform
itself was asked to stay untouched).

## Theming

`Source/Theme.{h,cpp}` — `Palette` is 11 `juce::Colour`s (window/panel bg,
waveform, accent, zero line, grid lines, playhead, loop/unsaved, record button,
text, dim text); `kPaletteFields[]` is a `{key, label, member-pointer}` table
that drives both the editor UI and the `"key:aarrggbb;..."` serialisation.

`ThemeManager` is a process-wide singleton via `juce::SharedResourcePointer` —
every painted component holds one, reads `theme->palette()` when it paints, and
listens to the manager (a `ChangeBroadcaster`) so a colour change repaints the
whole editor immediately. It persists the active palette and any user-saved
presets to `~/Library/Application Support/R3WRK/R3WRK.settings`
(`juce::PropertiesFile`, debounced writes), so the look is shared by every plugin
instance and the standalone — the plugin equivalent of RCRDR's `@AppStorage`
theme. Theme is **not** stored in the DAW plugin state.

Built-in "Start from" presets (in `Theme.cpp`): Midnight (the default look),
Slate, Graphite, Amber, Paper, Madrona (steel-blue chrome / dark waveform —
see below).

`Palette` has two text pairs: `text`/`textDim` for components with no fill of
their own (HeaderBar, EditorToolbar, KnobRow, R3WRKLookAndFeel — they show
through `windowBg`), and `screenText`/`screenTextDim` for the two components
that paint their own background (`WaveformDisplay`, `TimeRuler` — both fill
`panelBg`). Every preset before Madrona kept `windowBg`/`panelBg` close in
lightness, so one text pair read fine on both; Madrona deliberately doesn't
(a light steel `windowBg` around an untouched dark `panelBg`), which is why the
split exists — without it the ruler ticks and the "No audio loaded" placeholder
would render invisible dark-on-dark. `ThemeEditor` (Tools ▾ → "Theme…", shown in a
CallOutBox) has the preset combo, a hex row per colour (type a code or click the
swatch for an **in-panel** colour picker — deliberately not a nested call-out,
which would dismiss the parent), a name field + Save for custom presets, and
Delete / Reset.

## Sample-rate handling

The document stores audio at whatever rate it was recorded or loaded at.
Loading a file resamples it (via `juce::LagrangeInterpolator`) to the host's
current sample rate if they differ, so pitch/speed is correct in your DAW.

## Known gaps / natural next steps

- Recording is destructive-replace only (no overdub/punch-in/multiple takes).
- The spectrogram recomputes for the *whole* document on any audio-content
  change; fine for typical sample lengths, would want to be a smarter
  incremental/windowed computation for very long recordings.
- Keyboard shortcuts are limited to the editor essentials (Space, ⌘Z/⌘⇧Z,
  ⌘X/⌘C/⌘V); no user-configurable key map.
- The offline Stretch/Pitch edit (Tools menu) is "commit only" — no live
  audition before Apply. The knob-row Speed/Pitch *are* live but non-destructive
  (playback only); there's no "bake the knob settings into the clip" action yet.
- Restored plugin state is not resampled to the host rate (only file loads
  are), so loading a 44.1k session into a 48k project plays back pitched.
- The playhead runs ahead of the audible output by the real-time RubberBand
  latency while Speed/Pitch are engaged (no latency compensation on the marker).
- No AAX/LV2 build target configured (VST3 + AU + Standalone only), since
  those weren't asked for; JUCE supports adding them if you want them later.
