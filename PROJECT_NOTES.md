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

## Selection context menu + live Amplify/Stretch preview

Right-clicking inside a selection (`WaveformDisplay::onSelectionContextMenu`,
fired from `mouseDown` when `e.mods.isPopupMenu()` lands inside the selection
body) shows a small menu — Amplify…/Reverse/Stretch·Pitch… — handled by
`EditorToolbar::showSelectionContextMenu()` since it already owns the
`AmplifyPanel`/`StretchPanel` callouts and the `EditActions` calls Tools ▾ uses.
Amplify/Stretch open the same panels Tools ▾ does, just anchored at the click
point (`showAmplifyCallout()`/`showStretchCallout()` now take a
`juce::Rectangle<int> screenTargetArea` instead of always using
`toolsButton.getScreenBounds()`); Reverse runs immediately, same as Tools ▾.

Both panels also give a **live preview** while their slider is dragged, before
Apply — `AudioDocument` gained three plain (message-thread-only, not atomic)
fields for this: `previewActive`, `previewGainLinear`, `previewStretchRatio`.
`gain.onValueChange`/`stretch.onValueChange` set these and call
`notifyChanged()` (which reaches `WaveformDisplay` via the existing
`changeBroadcaster` listener — since it doesn't bump `bufferVersion`, that
listener's existing logic already skips the expensive full-path rebuild and
just repaints, so this reuses that path for free); each panel's destructor
resets them back to identity and notifies again, so the preview clears however
the panel closes (Apply, or dismissed by clicking away). Real edits
(`EditActions`) are completely untouched by any of this until Apply.

`WaveformDisplay::paintSelectionPreview()` draws the preview: a **fresh raw
copy of just the selection's samples** (not the whole-buffer peak cache the
committed waveform draws from — selections are typically far smaller than the
whole clip, so a raw copy stays cheap and gives full-resolution preview
regardless of zoom). Amplify scales the peaks in place, same pixel span as the
real selection. Stretch redraws that same audio spread across a wider/narrower
span starting at the selection's left edge — showing "this audio, once
stretched, would occupy this much room and roughly look like this" (the actual
semantics of what Apply will do), with a dashed line at the new right edge so
it reads as a preview, not the committed bound. Pitch has no visual (it
doesn't change the waveform's duration/shape here). No preview when there's no
selection (opening Amplify/Stretch from Tools ▾ with nothing selected — which
falls back to the whole clip — still works exactly as before, just without a
live preview).

**Bug found while testing this**: after stretching a selection, the *whole*
waveform looked stretched, and a freshly loaded file did too. Not this
feature's fault — `AudioDocument::loadFromFile()`/`newEmptyDocument()` never
reset the **KnobRow's** Speed/Pitch/Stretch knobs (a separate, pre-existing
feature — see "Visual time-stretch" above — that live-reshapes the *entire*
waveform's playback/display via `getTimeScale()`), so whatever position they
were left at from a previous file silently carried over into the next one.
Both functions now reset `playbackSpeed`/`playbackPitch`/`playbackStretch` to
identity (and the preview fields, for good hygiene) alongside the
selection/playhead/loop reset they already did — the KnobRow's own sliders
pick this up automatically via their existing 15 Hz `pull()` poll.

**Second bug found while testing this**: after an *extreme* Stretch, the view
was left showing only what used to be the whole file — the new, much longer
document was there, just off the right edge, with no obvious way back to the
full picture short of a lot of manual zoom-out. `viewStart`/`viewEnd` are
never touched by a length-changing edit on their own (reasonably — a user
zoomed into a sub-region editing carefully shouldn't get yanked out to full
view by an unrelated edit), so a `zoomToFit()`-equivalent had to be triggered
specifically for "the view *was* showing everything." `WaveformDisplay` gained
`lastKnownTotal` (the sample count as of `lastBufferVersion`) and
`refitViewIfContentChanged()`: whenever the buffer version changes, if the
view was at/covering `[0, lastKnownTotal)` just beforehand, it's reset to
`[0, newTotal)` too — otherwise left alone. Both `timerCallback()` and
`changeListenerCallback()` (the poll and the async-broadcast paths that used
to each do their own ad-hoc "viewBad" check) now call this shared helper
first; the pre-existing `viewBad` shrink-safety-net (view left dangling past a
now-*shorter* buffer, e.g. after Trim) still runs afterward, unchanged.

**Keyboard zoom.** `WaveformDisplay::zoomIn()`/`zoomOut()` (centred on the
current view, same ±2× step as one mouse-wheel notch) existed but were
unreachable from the UI since the header's Fit/zoom buttons were removed
(`63aa955`) — `PluginEditor::keyPressed` wires them to a Control shortcut, for
zooming without a mouse. Deliberately the literal Control key, not ⌘ —
`ModifierKeys::ctrlModifier` and `commandModifier` are distinct bits on macOS
(unlike Windows/Linux, where JUCE aliases them).

First attempt matched **Ctrl+"+"/Ctrl+"-"** on the resulting character
(`KeyPress::getTextCharacter()`) — built and shipped, then the user reported
"doesn't work". Root cause (read from JUCE's Cocoa backend,
`juce_NSViewComponentPeer_mac.mm::handleKeyEvent`): on a US Mac keyboard,
Control held with a *symbol* key never reaches `Component::keyPressed` at
all. Control has a defined control-code mapping for letters and a handful of
punctuation keys (`[`/`]`/`\`, etc.) but not for `-`/`=`/`+`, so Cocoa's own
text-input layer decides there's nothing to insert; `[NSEvent characters]`
comes back an empty string; and JUCE's handler only calls `handleKeyPress()`
(and even `handleKeyUpOrDown()`) by iterating *that* string — zero characters
means the loop body never runs, so the native key-down event is dropped
before any JUCE `KeyPress` is ever constructed. Not fixable by how the
`KeyPress` is matched in application code (nothing reaches it to match).

Second attempt: **Ctrl+Up/Ctrl+Down** instead — arrow keys don't hit the
symbol-key snag above (Cocoa always reports a non-empty character for them,
which is why JUCE's backend has to explicitly scrub it back out for arrow
keys a few lines further down — confirming the loop does run for them).
Matched the same way the existing ⌘Z/⌘X/⌘C/⌘V shortcuts are
(`key == KeyPress(KeyPress::upKey, ctrlModifier, 0)`), not by character. Built,
shipped — user reported "still not zooming". Root cause this time: Control+Up
and Control+Down are themselves claimed *system-wide* by macOS by default
(System Settings → Keyboard → Keyboard Shortcuts → Mission Control —
"Mission Control" and "Application windows"), intercepted well before any
app's `NSView` sees the event at all. A second dead end, unrelated to the
first.

Asked the user how to proceed rather than guess a third Control combo
(previous two both failed for platform reasons neither could have been
anticipated without hitting them) — chose **⌘+/⌘-**, the standard Mac zoom
convention (Safari, Preview, Xcode, ...), which sidesteps both problems: ⌘
doesn't share Control's missing-control-code-mapping gap, and isn't
system-reserved on the zoom keys. Matched **by keyCode, not by character** —
`key == KeyPress('=', commandModifier, 0)` (plus the `cmdShift` variant for
"+" arriving as Shift-⌘-"=", the usual case on a US keyboard, and a literal
"+" keycode variant for layouts that report one directly), the same idiom
the existing ⌘Z/⌘X/⌘C/⌘V shortcuts already use — necessarily so, since JUCE's
Cocoa backend zeroes the text character for every ⌘ chord (it's a command,
not text input), so `getTextCharacter()` was never going to work here either.
The Control-based code from both earlier attempts was removed rather than
left in as a fallback, since both are now confirmed dead on this platform.

Three follow-up refinements once ⌘+/⌘- actually reached the app: (1) each
press zoomed by a full ±2× (halving/doubling the view) — too coarse for a
single keypress, cut to a gentler ±(0.8/1.25) pair; (1b) still too much —
cut again to ±(0.9/1÷0.9) (`keyboardZoomStep` in `WaveformDisplay.cpp`, the
one constant to retune if a different step is ever wanted). Each pair is an
exact inverse of the other, so zooming in then out returns to the same
span; (2) made keyboard zoom behave
exactly like one mouse-wheel notch, not just a plain center-of-view zoom —
`zoomIn()`/`zoomOut()` now call the *same* `zoomToward()` the wheel handler
does, at the current mouse position (`currentMouseX()`, clamped onto the
component via `getMouseXYRelative()`). That gets `zoomToward()`'s existing
selection-aware behaviour for free: zooming in frames the whole selection
while the view is wider than it, and hovering near a selection edge pins and
zooms into *that* edge specifically — left or right, whichever the pointer
is nearest. The old plain `zoomBy(factor, centerSample)` (fixed at the view's
midpoint, no selection awareness) was removed as dead code once nothing
called it any more.

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

`WaveformDisplay` and `TimeRuler` both clip their `paint()` to a rounded
rectangle (`Path::addRoundedRectangle` + `Graphics::reduceClipRegion`, radius
10, matching the control band) — the waveform's own rendering is otherwise
untouched, only the panel's outer shape rounds. The transport row groups
Play/Loop/Record/Tools on the left with the time readout pinned to the right
(dropped the separate "● REC" label; `timeLabel` carries that text and turns
`playhead`-coloured while recording instead).

The plugin's own outer window/editor bounds are **not** rounded and won't be —
in a host the editor is a plain rectangle the host draws its own frame around,
and reshaping an actual OS window for the standalone isn't worth the
fragility. Only panels drawn by R3WRK itself can round, and now do.

Not yet covered: `ComboBox`/`PopupMenu` (ThemeEditor's preset picker, the Tools
menu's own popup) still use the default JUCE look.

### Transport icons (Play/Loop/Record)

Play, Loop and Record are now square (28×28) icon buttons instead of wide
text pills — square bounds mean `drawButtonBackground`'s existing
`radius = height/2` pill formula draws a true circle, so no new
background-drawing code was needed, just narrower `resized()` widths.

The icons themselves are hand-drawn vector paths, not font glyphs — most
"two arrows in a circle" Unicode loop glyphs (🔁/🔄) render as fixed-colour
emoji and ignore `Graphics::setColour`/JUCE text-colour, which would break
per-theme tinting. Instead `R3WRKLookAndFeel::drawButtonText` checks the
button's text against three sentinel marker strings (`iconPlay`/`iconStop`/
`iconLoop`, static constants on the class) and draws a path instead of text
when it matches, falling back to normal JUCE text rendering for anything
else (so the one override is safe for every button, e.g. `toolsButton`'s
literal "Tools ▾"). `playButton`/`loopButton` swap between the play-triangle
and stop-square markers depending on transport state; `recordButton` carries
no icon at all while idle — a plain red circle already reads as "record" —
and shows the stop-square once recording starts.

The loop icon is two ~140° arcs (`Path::addCentredArc`) with gaps between
them, each ending in a small triangular arrowhead computed from
`Point::getPointOnCircumference` plus the tangent/normal at that angle.

A fourth, `playFromStartButton`, sits to the left of Play (same 28×28
circular shape, outlined rather than filled -- it's a secondary/modifier
action, not a primary one): a vertical bar next to the same play triangle
("go back to the start, then play forward"). Its handler
(`EditorToolbar::playFromStart()`) forces `document.playhead` to 0 and
restarts playback; `startPlayback()`/`processBlock`'s existing region
snap then lands it at the start of whatever's actually playing -- the
selection if there is one, else the loop range, else sample 0 -- with
no extra region-lookup logic duplicated in the toolbar.

`toolsButton` (still opens the same Tools ▾ pop-up menu) also dropped its "Tools ▾"
text for a drawn icon (`iconTools`), in the same 28×28 outlined-circle treatment as
Play-from-start. First pass was a crossed screwdriver + open-end wrench (from a
user-supplied reference image) -- rejected ("don't like the way that looks") after
looking like a scribble even simplified down to one terminal shape per tool.
Replaced with a plain gear/cog: one `Path`, traced as a single polygon around the
body+teeth perimeter (`Point::getPointOnCircumference` at each tooth's left/right
edge and the gap after it, walking monotonically around the circle so consecutive
teeth share their boundary point with no unioning needed). First pass filled the
polygon solid with the centre hole punched out via even-odd fill
(`Path::setUsingNonZeroWinding(false)`); the user then supplied a reference gear
glyph in a thinner, hollow line-icon style, so it's now **stroked** instead
(`PathStrokeType::curved` rounds the tooth corners for free) with a separately
stroked circle standing in for the hole. Getting it legible at 28px took two
rounds of screenshot+zoom iteration: 8 teeth at a half-width too close to the
22.5° half-step first overlapped into a solid octagon; once switched to the
stroked style, the ring band between body and hole radius also needed to be
wider than the stroke thickness or it filled in solid again -- settled on body/
hole/tooth-length radii with enough clearance at the (near-floor, ~1.3px) stroke
thickness this size forces. Tooth count was fixed 8→6 to match the reference.

Asked to "get it right" against the reference exactly, the hand-tuned version
was replaced with an actual trace of the source image: the user's reference PNG
(44×44, `~/Desktop/Screenshot ... .png`) was radially ray-cast in Python/Pillow
from its ink centroid -- for angles at 7.5° steps, walking outward in 0.05px
steps and recording where the pixel luminance crosses a dark/light threshold --
giving the exact outer (tooth) and inner (ring) radius at each angle, plus the
centre hole's inner/outer radius. The source turned out to be exactly 6-fold
symmetric (the 8 samples-per-60°-sector data is consistent to ~1% across all
six repeats), so the six sectors were averaged into one canonical 8-point
profile and hardcoded as `kOuterR`/`kInnerR` ratio arrays (of the tooth-tip
radius) in `drawGearIcon`, repeated six times at render time. This is why the
gear's ring isn't a constant width -- it genuinely pinches in at each tooth tip
and at each valley and swells on the flanks between, in the source icon and now
in ours, traced rather than smoothed into a uniform stroke. Each ring (gear body,
centre hole) is built as an outer contour (clockwise) plus an inner contour
(counter-clockwise) so nonzero-winding `fillPath` punches the band out directly,
rather than using `PathStrokeType` at all.

All five toolbar buttons (`playFromStartButton`/`playButton`/`loopButton`/
`recordButton`/`toolsButton`) call `setWantsKeyboardFocus(false)`. A mouse
click always leaves keyboard focus sitting on whichever button was
clicked, and on macOS a focused button's own "press" accessibility action
fires again on Space (Full Keyboard Access) -- so clicking, say, Loop and
then hitting Space to toggle playback would re-toggle Loop instead of
reaching `PluginEditor::keyPressed`'s Space handler. This is a transport
strip, not a tab-navigable form, so keeping these off the focus chain
lets a click's focus grab bubble straight up to `PluginEditor` (which
does want keyboard focus) instead, and Space reliably means "toggle
play" no matter what was last clicked.

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
  ⌘X/⌘C/⌘V, ⌘+/⌘- to zoom the waveform); no user-configurable key map.
- The offline Stretch/Pitch edit (Tools menu / selection right-click) commits
  audio only on Apply — the slider drag previews *visually* (see "Selection
  context menu + live Amplify/Stretch preview" above) but there's no live
  *audition* (hearing it) before Apply. The knob-row Speed/Pitch/Stretch are
  live but non-destructive (playback + visual only); there's no "bake the knob
  settings into the clip" action yet.
- Restored plugin state is not resampled to the host rate (only file loads
  are), so loading a 44.1k session into a 48k project plays back pitched.
- The playhead runs ahead of the audible output by the real-time RubberBand
  latency while Speed/Pitch are engaged (no latency compensation on the marker).
- No AAX/LV2 build target configured (VST3 + AU + Standalone only), since
  those weren't asked for; JUCE supports adding them if you want them later.
