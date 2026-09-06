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

**Bug: "can't zoom out to see the whole file" at a large Stretch/Speed.** Two
rounds fixing the same user report. The first round (`cdbc1c4`) assumed the
problem was `viewStart`/`viewEnd` not re-expanding after an edit that grows the
document (an extreme *destructive* Stretch/Pitch, via Tools ▾ / a selection
right-click) — real, and fixed, but the user was actually driving the
**KnobRow's** live Stretch knob (0.25–50×, non-destructive, no document-length
change at all), which that fix didn't touch. Turned out there's a second,
independent bug underneath: dividing samples-per-pixel by `timeScale` (above)
means a view of exactly `[0, rawTotal)` only actually *renders*
`[0, rawTotal/timeScale)` — the render loop reaches the component's fixed
pixel width well before it has consumed all of `rawTotal`'s raw samples, so
the rest is silently never drawn, **no matter how far out you "zoom"** (every
zoom/pan clamp capped the view span at `rawTotal`, which was never wide enough
to begin with once `timeScale > 1`). Confirmed by deriving the pixel math by
hand and cross-checking a real, timeScale=18.5 session (a restored-on-relaunch
standalone with a leftover 18.5× Stretch, `getTimeScale()` persists via plugin
state) via screenshot: before this fix, only the first ~5% of that clip's
audio would ever have rendered, filling the whole width; the time ruler's
labels — genuinely timescale-invariant, unaffected by any of this — read out
to 8s × 18.5 ≈ 2:28, confirming what *should* be visible.

Fix: `WaveformDisplay::maxViewSpan()` (`effectiveSpanFor(rawTotal, timeScale)`,
`= rawTotal · max(1, timeScale)`) replaces plain `document.getNumSamples()`
everywhere a **view span or its upper bound** is computed — `zoomToFit()`,
`zoomToward()`'s span clamp and its `applyView` position clamp, `panByPixels()`
(also fixed a live `jlimit(low, high)` with `high < low` once span could
exceed `rawTotal` — was one dropped frame from an assert/UB), and the
`viewBad`/full-view-refit checks in `timerCallback()`/`changeListenerCallback()`.
Sample-*indexing* uses (`xToSample()`'s clamp, `setSelection(0, getNumSamples())`
for select-all) are deliberately untouched — you still can't select or seek
past real audio, only the *view* can extend into blank space beyond it,
mirroring the already-correct `timeScale < 1` case (a view of `[0, rawTotal)`
there already shows real content only part-way across, blank for the rest —
this just extends that same idea, symmetrically, to the other side).

The KnobRow case needed one more piece: turning a knob writes straight to an
atomic (`document.playbackStretch.store(v)`, no change broadcast), so
`WaveformDisplay`'s 30 Hz timer poll is the *only* place a knob move is ever
noticed at all — `timerCallback()` now also re-runs the "was showing
everything, so keep showing everything" check whenever `getTimeScale()`
itself changes (not just when `bufferVersion` does), using `lastTimeScale`
(already tracked, previously only used to know a rebuild was needed) alongside
the *old* `maxViewSpan()` for the comparison.

**Follow-up: made it symmetric.** The fix above only *widened* the span for
`timeScale > 1` (`effectiveSpanFor` floored the multiplier at `1.0`), so
`timeScale < 1` (speeding up) kept the older, merely-not-broken behaviour:
a "fully zoomed out" view of `[0, rawTotal)` renders all of `rawTotal` well
before reaching the right edge, leaving the rest of the component's width as
dead, empty space — not wrong, exactly, but not what the user wanted either
("make it do the same thing when speeding up... the timeline fits the
waveform window"). Dropping that floor makes `effectiveSpanFor` symmetric:
`rawTotal * timeScale` in *both* directions. At `timeScale < 1` this actually
*shrinks* the span below `rawTotal` -- which sounds like it should lose
content, but doesn't: `samplesPerPixel = rangeLen/width/timeScale` with
`rangeLen = rawTotal*timeScale` simplifies to exactly `rawTotal/width`
regardless of `timeScale`, so the last pixel's cursor lands exactly on
`rawTotal` either way -- the whole raw buffer, no more, no less, every time.
One side effect, intentional given the ask: the zoom-out clamps (`zoomToward`,
`panByPixels`) now use this as a **hard** cap in both directions, so it's no
longer possible to manually zoom out *past* "exactly fits" to see blank space
on purpose -- consistent with "the timeline [always] fits the waveform
window" rather than that being merely the default. Verified against a real
restored session (persisted Stretch=0.25×, an 8 s file) via screenshot: the
waveform now fills the window edge to edge with real detail (ruler reading
0:00–~1.9 s, matching 8 s × 0.25 exactly) where it previously left most of the
width blank.

## Live real-stretch waveform preview (`WaveformStretchPreview`)

While explaining the visual-time-stretch trick above, the user asked the
natural follow-up: doesn't stretching change the waveform *shape*? — it still
looked the same, just wider. Correct — everything above is a *rescale* of the
stored audio's peaks, not a rendering of what a real time-stretch actually
does to a signal (transients smear/soften at extreme ratios; the true output
isn't just the same shape spread over more pixels). Asked whether to make the
live KnobRow preview show the *actual* processed shape instead; yes.

`Source/WaveformStretchPreview.{h,cpp}` (new, `WaveformDisplay` owns one):
while Speed/Pitch/Stretch are non-identity, runs the **same offline RubberBand
pass the destructive Stretch/Pitch tool uses** (`TimeStretchEngine::process`
— not a separate/cheaper algorithm) over the *whole* document, on a
background `juce::Thread`, and delivers the result back for
`rebuildWaveformPath()` to draw instead of the rescaled original. Key pieces:

- **Debounced** (`debounceMs = 250`): a knob has to sit still before a
  recompute starts, so a drag doesn't launch a RubberBand pass on every pixel
  of motion. `update()` (called from `WaveformDisplay`'s existing 30 Hz timer,
  alongside the existing timeScale/bufferVersion polling — KnobRow writes its
  atomics directly with no change broadcast, so this poll is the only place a
  knob move is ever noticed at all) tracks when the knobs last changed and
  only signals the worker once they've been still for the debounce window.
- **Background thread**, not a one-shot job: a `juce::WaitableEvent`-driven
  `juce::Thread` that wakes on a signal, claims whatever request is current,
  runs the (blocking, can take real time on a long file at an extreme ratio)
  offline stretch, and stores the result for the message thread to pick up
  next `update()`. No attempt to interrupt a pass already in flight if a newer
  request supersedes it mid-computation — `TimeStretchEngine::process` has no
  cancellation hook to thread through, so a superseded result is just left
  undelivered and the worker immediately starts on the newest request once it
  loops back round instead.
- **Same time-ratio / pitch-scale mapping the real-time playback engine uses**
  (`renderPlaybackStretched`): `timeRatio = stretch/speed`, and since the
  offline engine's `pitchSemitones` parameter is semitones while the
  real-time engine's is a pitch-scale ratio, `semitones = 12·log2(speed) +
  pitch` converts between them — so the preview matches what you'll actually
  hear, not merely something in the same direction.
- `WaveformDisplay::rebuildWaveformPath()` change is small: a raw sample
  position `s` (still computed exactly as before) maps to processed-domain
  index `s * timeScale` when a preview is available (same factor that already
  rescales the view span itself, see `effectiveSpanFor()` above), reading
  peaks via `stretchPreview.getPeakRange()` instead of the raw peak-cache/
  deep-zoom-copy path — `xToSample()`/`sampleToX()` (selection, playhead,
  loop) are completely untouched, since those are raw-sample concepts
  independent of which buffer supplies the drawn shape.
- Verified end-to-end with a real regression test (`SmokeTest.cpp`,
  `WaveformStretchPreview` now also linked into the headless `R3WRKSmokeTest`
  target — it's pure `juce_core`/`juce_events` logic, no GUI dependency)
  exercising the actual debounce → background thread → RubberBand → peak-cache
  → delivery pipeline, not just the math: identity knobs never produce a
  preview; a real Stretch=3x eventually delivers a preview ~3x as long, with
  the channel count preserved and the peak cache holding real (non-silent)
  signal; returning to identity clears it again.

**Bug found immediately after shipping the first version**: the user reported
that once the preview appeared (after the expected multi-second wait for the
background RubberBand pass, which they were fine with), they could no longer
make selections, and repeatedly pressing Play-from-start sounded like
playback just kept going rather than restarting. Root cause: the *first*
version handed the raw processed `AudioBuffer` back to the message thread,
which then scanned it directly (a plain `findMinAndMax` per pixel column,
same as the existing deep-zoom fallback) inside `rebuildWaveformPath()` —
fine for the original few-second test file, but at an extreme ratio on a
longer clip the processed buffer can run to tens of millions of samples per
channel, and scanning that synchronously **on the message thread** blocks
mouse-event dispatch and click handling for as long as the scan takes, on
top of the RubberBand pass itself. That single blocking scan is consistent
with both symptoms: a drag-to-select straddling that window reads as "can't
select", and a Play-from-start click landing inside it reads as "didn't
restart" (the click is delayed/queued behind the block, not lost, but it
certainly doesn't feel like a restart).

Fixed by moving the peak-binning onto the **background thread**, right after
the RubberBand pass, mirroring `WaveformDisplay::rebuildPeakCache()`'s
approach for the raw buffer (`WaveformStretchPreview::peakBinSize`, matching
`WaveformDisplay`'s own constant): the worker delivers a compact min/max
cache (`getPeakRange()`), not the raw processed audio, and the *raw processed
buffer is discarded* once binned rather than kept around (real memory
savings too, at an extreme ratio). The message-thread side of a rebuild is
now the same bounded cost as the already-fine raw-buffer path, however long
the actual processed audio is — mouse/UI events are never blocked by it,
whatever the stretch ratio.

Known limitation, accepted for v1: the destructor still blocks
(`stopThread(10000)`) waiting for an in-flight RubberBand pass to finish
rather than force-killing it, since there's no cancellation hook — a real
risk only for a very long clip at an extreme ratio outliving the editor
being closed, which isn't the expected use case for a sample editor.

**Related bug, surfaced by testing the above**: after the message-thread-
blocking fix, the user reported selections were still "hit and miss" at a
high Stretch — reliable zoomed in, unreliable (especially for *small*
selections) zoomed out. Root cause was in `WaveformDisplay::mouseUp()`'s
click-vs-drag test, not the preview feature: every *other* pixel↔frame
conversion in this file (`xToSample()`, `sampleToX()`, `panByPixels()`,
`zoomToward()`) divides by `getTimeScale()`, but this one — computing `slop`,
the "was that actually a drag, or just a click" tolerance — didn't. At a
high Stretch the *real* frames-per-pixel (what `xToSample()` actually uses)
is much smaller than this uncorrected version computed, so `slop` came out
inflated by roughly a factor of `timeScale` — easily swallowing a genuine
small drag as "just a click" and clearing the selection instead of keeping
it. Zoomed out made it worse (`viewEnd - viewStart` — and so the error — is
largest there, at `maxViewSpan()`); zoomed in happened to still work because
the absolute error stayed small enough not to matter. Not a fluke of this
session's other fixes — timeScale != 1 is the trigger, so this bug already
existed wherever Speed/Pitch/Stretch were live, just went unnoticed until
this round of testing exercised it directly.

**Keyboard zoom stays anchored on the selection.** `zoomToward()`'s existing
Sieve-ported framing behaviour (frame the whole selection, centred, while the
view is wider than it) only fires for the *first* zoom-in step — once you're
zoomed in tighter than the selection, it hands off to plain pointer-anchored
zoom, which is exactly right for the mouse wheel (the pointer is genuinely
pointing at something) but wrong for ⌘+/⌘-, where the "pointer" was just
whatever the mouse happened to be sitting over — often nowhere near the
selection, since the whole point of a keyboard shortcut is not needing the
mouse there. Every zoom-in past the first press would then anchor on that
arbitrary spot instead, drifting the view away from the selection with each
further press — the exact "zooming around trying to find the selection
again" experience the Sieve behaviour exists to avoid. Fixed by giving
`zoomIn()`/`zoomOut()` their own anchor (`keyboardZoomAnchorX()`): the
selection's midpoint, converted to a screen X via `sampleToX()`, whenever
there is one, falling back to the real mouse position only when there's no
selection to anchor on. Since this "anchor" is just the `pointerX` fed into
the same `zoomToward()` the wheel uses, it gets the framing behaviour *and*
the pointer-anchored zoom that follows it for free, both now centred on the
selection instead of an incidental mouse position, at every zoom level.

**Deliberate zoom-out direction around a selection.** User: zooming in on a
selection "zooms in from the left, then when I get close... zooms really
fast to the selection. Then when I zoom out, it zooms back out to the left,
no matter if my mouse pointer is on the left or right" — wanted to be able
to reveal either side of a selection just by choosing where to point before
scrolling out. Root cause: the bracket-grab lock-on (pin whichever selection
edge is within 12px of the pointer, at a fraction *derived from that same
on-screen position*, clamped to at least 0.15) used to fire for **both** zoom
directions. Zooming in near an edge, that's the intended precision-dive
behaviour ("so the wheel pulls you into it"); but once it had you hugging the
left bracket and you reversed to zoom back out without moving the mouse
(which hadn't needed to move — it was already sitting right at that edge),
the *same* "near the left bracket" case kept re-triggering and re-pinning
that edge near the left of the screen again, regardless of intent — the
"drifts left no matter where the pointer is" the user described, since once
the case matches, the pointer's exact position barely changes the outcome.

Fixed by gating bracket-grab (and the "frame the whole selection" step after
it) to zoom-**in** only (`spanFactor < 1.0`), and giving zoom-out its own,
deliberate branch instead: with a selection present and not zooming in, pin
whichever *edge* is nearer the pointer's screen **half** (not its exact
position, so it can't degenerate into the same edge-hugging problem) at a
fixed 0.7/0.3 fraction of the width — pointer on the left keeps the selection
start pinned there and opens up what's *before* it as you keep scrolling out;
right does the same for the selection end and what's *after* it. Deterministic
and repeatable on every wheel notch, not just a one-time jump.

**Two follow-ups from trying the above**: "It's kind of working... Could you
make it so when I put my mouse pointer in the middle of the selection, it
zooms into that mouse pointer. It still wants to come from one side or the
other. Also, please slow down the speed of zooming."

1. The zoom-*in* "frame the whole selection" step (above) always centred on
   the selection's exact midpoint, regardless of where the pointer actually
   was within it — so every zoom-in step (there are several before the view
   gets narrower than the selection) snapped to the same fixed point no
   matter where you aimed, reading as "it still wants to come from one side
   or the other". Now anchored on the pointer's actual position
   (`xToSample(pointerX)`) whenever it's inside the selection's bounds, only
   falling back to the exact midpoint when the pointer is outside it (zooming
   in on the selection from elsewhere, where there's no more specific spot to
   prefer).
2. Wheel-zoom sensitivity (`mouseWheelMove()`) was
   `jlimit(0.5, 2.0, exp(-dy*1.4))` — a single wheel event could as much as
   halve or double the view, which on a trackpad's usual burst of events per
   swipe added up to "jumps quickly to a very small bit" long before the
   gesture felt finished. Tightened to `jlimit(0.8, 1.25, exp(-dy*0.6))` —
   both the per-event ceiling and the `dy` sensitivity are gentler (0.8/1.25
   is an exact inverse pair, same idea as `keyboardZoomStep`). Keyboard zoom
   (⌘+/⌘-) already had its own separate, gentler step and wasn't touched.

**Still not right — the actual bug was the "frame the selection" jump.**
User: "I can't figure this out. If I put my mouse pointer in the middle of
the selection from a zoomed out position and zoom in a tiny bit, jumps into
full zoom of the selection." The zoom-in framing step (previous entry) keyed
off `curLen > selLen` — true for nearly the *entire* zoomed-out range above
a small selection in a longer file, not just the last step before naturally
reaching that size — so `framedLen = jmin(newLen, selLen*1.2)` picked
`selLen*1.2` immediately on the very first zoom-in tick, however far zoomed
out the view started. **Removed the special "frame the selection" branch on
zoom-in entirely** rather than re-tuning its trigger again: it now falls
through to the same plain pointer-anchored zoom used everywhere else, which
(per the fix directly above) already tracks the pointer correctly whether
it's inside the selection or not — gradual and predictable, matching how
zoom behaves everywhere else in the app, with no special case left to get
the threshold wrong a third time. Bracket-grab (zooming in right at an edge)
is untouched; zoom-out's deliberate side-of-selection anchoring is untouched.

**Locking the zoom anchor for the whole gesture.** Follow-up ask: "I
understand that if I zoom in that the mouse pointer is going to change
location, but isn't there a way where it can remember that I wanted to zoom
into the place I wanted to zoom in in the beginning?" A trackpad/wheel
gesture sends a rapid burst of small events, and `zoomToward()` was reading
the pointer's live x on every single one — incidental mouse jitter during
the gesture (no one's hand is perfectly still) nudged the anchor a little
each notch, so a many-notch zoom could drift from where the gesture actually
started by the time it finished. `WaveformDisplay` now locks
`wheelGestureAnchorX` to wherever the pointer was when a gesture *starts*
(no wheel event for more than `wheelGestureGapMs` = 400 ms) and reuses that
same x for every notch until the next pause, rather than re-reading `e.x`
each time — `zoomToward()` itself is unchanged, it just always receives a
steady anchor for the duration of one gesture instead of a jittery one.
Panning (horizontal swipe) has no anchor concept and isn't affected.

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

## Scrub tool

User: "I'd like for you to add a function to R3WRK that when you press a
button, you can scrub through the waveform with your mouse and it goes
forward or back depending on where you scrub through. I want the sound to be
like a tape player speeding up and slowing down, depending on how fast you are
scrubbing through." A toggle button (`EditorToolbar::scrubButton`, cassette-
tape icon, styled/behaved exactly like the Loop toggle) puts the waveform into
a mode where every mouse gesture there is repurposed for scrubbing instead of
selection editing.

**Deliberately not RubberBand.** Every other rate/pitch-changing feature in
R3WRK (Speed/Pitch/Stretch, the offline Stretch/Pitch edit) goes through
RubberBand specifically to *decouple* pitch from rate. Scrub is the opposite
ask — the pitch rising and falling with drag speed *is* the effect being
asked for, the same way a real tape or turntable does it under your hand — so
`PluginProcessor::renderScrub()` is a plain linear-interpolated, variable-rate,
reversible read of the stored buffer: `document.scrubVelocity` (signed, raw
samples/sec) is integrated into a fractional read cursor (`perSample =
velocity / sampleRate` per output sample), with linear interpolation between
the two neighbouring samples at each fractional position. No separate DSP
mode to maintain, no RubberBand latency, and it's exactly the analog behaviour
being asked for rather than an approximation of it.

**Where the velocity comes from.** `WaveformDisplay` re-derives velocity on
every `mouseDrag`: the sample under the pointer now, minus the sample under
the pointer at the last event, divided by the wall-clock time between them
(`juce::Time::getMillisecondCounterHiRes()`), clamped to a generous but finite
±12 seconds-per-second-of-audio ceiling so a fast flick can't send the read
cursor rocketing off in one block. `mouseDown` seeds `document.playhead` at
the press point and starts silent (`scrubVelocity = 0`) until the drag
actually moves; `mouseUp` stops it dead. `mouseMove` (not dragging) just shows
a left/right-resize cursor as a "drag to scrub" affordance; `mouseDoubleClick`
is a no-op while scrubbing is the active tool, since select-all doesn't mean
anything here.

**Wiring into `processBlock`.** A new priority tier sits between recording and
normal playback: `document.isScrubbing` (true only while the mouse is actually
down and dragging, distinct from `document.scrubModeEnabled`, which just means
the tool is selected) short-circuits the block to `renderScrub()` and returns,
so normal playback and scrubbing never run in the same block. Edge-detection
(`wasScrubbing`, mirroring the existing `wasPlaying` pattern) re-seeds
`scrubReadPos` from `document.playhead` the instant a drag begins, so each new
drag gesture starts from wherever it was pressed rather than continuing from
the last gesture's end point. Turning the tool off (un-toggling the button)
clears `isScrubbing`/`scrubVelocity`; toggling it *on* stops any normal
playback in progress first, since the two don't mix.

**Icon.** Initially planned as a double-headed arrow; the user sent a
reference image ("this is the icon for the button") of a cassette tape, so
`drawScrubIcon()` traces that instead — a stroked rounded-rectangle body with
two reel circles sitting toward the bottom and the tape strung along their
lower tangent, matching the gear icon's stroked-line style rather than a
filled glyph. (Briefly worried, from a small screenshot crop, that the button
was rendering "filled" instead of the outlined style Tools/Play-from-start
use — pixel-sampled the actual rendered button to check rather than trust the
eyeball: the sampled centre colour matched Play-from-start's exactly,
confirming the toggle/colour code — copied from the already-correct Loop
button — was fine all along; what read as "filled" at a glance was just the
icon's own stroke lines sitting dense inside a small circle, not a background-
colour bug.)

Not unit-tested in `SmokeTest.cpp`: `PluginProcessor` (and so
`renderScrub()`) isn't linked into the headless `R3WRKSmokeTest` target at
all, consistent with `renderPlaybackDirect`/`renderPlaybackStretched` also
having no smoke-test coverage — this is real-time audio-thread code exercised
by ear, not something the headless harness can drive.

**Fixed: velocity model was wrong, redone as a shuttle control.** First test:
"Right now it's just scrubbing really fast no matter how you pull." The v1
`mouseDrag` computed velocity as pixel-delta-since-last-event divided by
time-since-last-event — literal mouse speed. At any real zoom level,
samples-per-pixel is large enough that even a slow drag's per-event pixel
delta represents a huge sample delta, and dividing that by a ~10ms
event-to-event gap produces a velocity far past the ceiling almost
immediately — reads as "always maxed out" no matter how gently you pull.
User's clarification of the intended feel: "you put down the mouse pointer
and drag to the right or left and it starts out slow, no matter how fast you
pull it. Then once it starts, very slow, you can speed it up by pulling it
further in either direction" — a shuttle-wheel control (tape deck / NLE JKL
shuttle), not a literal per-instant drag speed. Redone: `mouseDown` drops an
anchor (`scrubAnchorX`, screen-space pixels, so it's independent of zoom)
at the press point; `mouseDrag` now derives velocity purely from *how far*
the pointer currently sits from that anchor — `magnitude = clamp((|offset|
- deadZone) / (maxDrag - deadZone), 0, 1)`, squared before scaling to the
velocity ceiling, so it's slow near the anchor and ramps up (not linearly —
faster) the further out you go, holding steady at whatever rate corresponds
to the current distance for as long as you keep the pointer there, in
either direction depending on which side of the anchor you're on. Because
it's purely a function of *position*, not motion, a fast flick to a modest
distance now starts exactly as slow as a careful pull to that same
distance. Smoke test passes; all four targets build clean.

**Less sensitive.** User: "Almost. Could you make it less sensitive?" — the shape (slow near the
anchor, faster further out) was right, just reached full rate too easily. `scrubMaxDragPx`
(160 → 400) is the one knob that controls this: it's the pull distance the whole 0→1 curve is
stretched across, so doubling-plus it means the same physical drag now lands earlier/slower
along that curve everywhere, not just at the top end. Smoke test passes; all four targets build
clean.

**Moved to the end of the row, reshaped to a rectangle.** User: "I'd like the button for
scrubbing moved to the last on the right and I would like you to make it a rectangle not a
circle, so it can look like the image I gave you." Reordered `EditorToolbar::resized()`'s
`FlexBox` items: Scrub now comes after Tools (the last item before the flexible gap that pins
`timeLabel` to the far right), instead of sitting between Loop and Record. Shape needed no
`R3WRKLookAndFeel` change at all: `drawButtonBackground`'s pill formula is already `radius =
height/2` on whatever bounds the button has, which draws a true circle only because every other
icon button happens to be square (28×28) -- giving Scrub a wider `FlexItem` width (28 → 46, same
height as the rest) makes that identical formula draw a stadium/rounded-rectangle shape on its
own, matching the reference image's cassette-body proportions. `drawScrubIcon` needed no changes
either -- its margins are already computed from height alone, so the wider bounds just gives the
cassette body a wider, more authentic aspect ratio than the cramped square version. Smoke test
passes; all four targets build clean.

**Corners squared off.** User: "can you make the corners of the scrub button less rounded?"
`drawButtonBackground`'s pill formula (`radius = height/2`) is shared by every icon button, so
narrowing it just for Scrub needed a one-button exception rather than a formula change --
`drawButtonBackground` now checks `button.getButtonText() == iconScrub` (the same sentinel-marker
dispatch `drawButtonText` already uses to pick an icon) and uses a much shallower `height*0.22`
radius for it alone, leaving every other button's full stadium curve untouched. Reads as a proper
cassette case corner now rather than a stadium with a cassette drawn inside it. Smoke test passes;
all four targets build clean.

**Icon replaced with a reel hub; button back to a circle.** User: "Don't like the scrub button.
Can we use this one and go back to a circle?" — supplied a new reference image (`~/Desktop/Tape
2.jpg`): a thick ring whose inner hole isn't a smooth circle but a 6-point spline, six
rectangular notches cut inward every 60°, the sprocketed drive-hole a tape deck's spindle grips
to turn a cassette reel. Radial ray-cast the reference the same way as the gear icon earlier
(walk outward from the ink centroid at fine angle steps, record dark/light transitions) —
confirmed 6-fold symmetry (notches at 60° spacing) and two radii: the outer ring boundary is a
plain circle throughout, the inner hole alternates between a shallow radius (≈0.79× outer,
between notches) and a deep radius (≈0.59× outer, at each notch), each notch a straight-edged
rectangular cut rather than a curve. `drawScrubIcon` rewritten to build exactly that: an outer
circle (clockwise) plus a 6-notch hole contour (walked counter-clockwise), combined into one
`Path` and filled with nonzero winding — the same "outer contour clockwise + inner contour
counter-clockwise punches a hole" technique `drawGearIcon` already used for its body/centre-hole,
just with a spline hole instead of a round one. Reverted both button-shape changes from the
rectangle experiment: `EditorToolbar::resized()`'s `scrubButton` width back to 28 (square, so the
existing pill formula draws a true circle again) and removed `drawButtonBackground`'s
Scrub-specific shallow-corner-radius exception entirely — the reel-hub icon reads better in a
circle than the cassette-body icon did. Stayed last in the row (that part of the prior feedback
holds). Smoke test passes; all four targets build clean.

## Reverse button

User: "Can you make a button for reverse? Round button with arrow going to the left."
`EditActions::reverse()` already existed (reverses `getEffectiveRange()` -- the selection, or
the whole clip if there's none -- used by Tools ▾'s "Reverse" item and the selection right-click
menu), so this is purely a new toolbar shortcut to it: a round icon button, last in the row after
Scrub, same outlined (not filled/toggling) treatment as Tools/Play-from-start since it's a
one-shot action with no on/off state of its own. `onClick` just calls
`EditActions::reverse(document)` directly -- no extra `notifyChanged()` needed,
`AudioDocument::commitChange()` already broadcasts the change. Disabled while recording, same as
Scrub. New `drawReverseIcon`: a triangular arrowhead at the left joined to a horizontal shaft --
deliberately a *drawn arrow*, not a plain mirrored copy of Play's triangle, so it doesn't read as
"play backwards" (a different, not-implemented feature) at a glance. Smoke test passes; all four
targets build clean.

## Clear button

User: "Now make a clear waveform (which sets all parameter's back to default). Use this for the
button" -- supplied a reference image (a red X in a circle, the familiar macOS "clear field"/
close glyph). `AudioDocument::newEmptyDocument()` already did almost exactly this (empties the
buffer, resets Speed/Pitch/Stretch to identity, clears selection/loop/playhead, clears undo
history) -- it just wasn't reachable from anywhere except indirectly (a fresh recording, or
loading a new file, calls it internally). New `clearButton`, last in the row after Reverse:
`onClick` stops playback if running, then calls `newEmptyDocument()` with the *current* channel
count/sample rate (this is "start over with a blank canvas", not "close the file" -- unlike a
`Cmd+N`-style "new project" flow that resets to a fixed default channel count/rate), then fires
`onSourceNameChanged({})` (the same "" a fresh recording sends) so the header goes back to
showing an untitled/unsaved state. New `drawClearIcon`: two crossing diagonal strokes, rounded
caps, at the gear/reel icons' stroke weight -- and since Clear is destructive (no confirmation
dialog today), its ink uses the same red as the record button rather than the neutral
screenText every other outlined icon gets, a quiet "careful" cue without going as far as a
filled warning circle. Disabled while recording, same as Scrub/Reverse. Smoke test passes; all
four targets build clean.

(Also carried over from a still-open, unconfirmed experiment: the transport row's inter-button
gap is currently 16px, up from the original 4px, tried live at the user's request to preview
wider spacing -- not yet explicitly confirmed as final.)

## Auto-Record

User's plan, verbatim: "I'd like to make a button for 'record when audio input starts'. In the
settings I'd like a way to adjust the dB that R3WRK starts recording. Before that record
pauses." A level-triggered record standby -- arm it, and R3WRK starts recording for real only
once the input peaks past a configurable dBFS threshold, the same idea as a tape deck's
voice-activated record or a field recorder's auto-record.

**Why the audio thread never calls `startRecording()` itself.** Every other toggle in this class
(Loop, Scrub) only ever has the *message* thread write real state and the *audio* thread read
it -- `startRecording()` allocates (`recordingAccumulator.setSize(...)`) and touches
document/transport state meant to be driven from outside the audio callback, so triggering it
directly from inside `processBlock` would be a new, one-off exception to that rule. Instead:
`AudioDocument` gained `autoRecordEnabled` (the toggle being armed, atomic since the audio
thread reads it every idle block), `autoRecordThresholdDb` (atomic double, -60..0, default -40,
written by the new threshold panel), and `autoRecordTriggered` (atomic bool) -- the audio
thread's only job, in `processBlock`'s existing idle/pass-through branch (neither recording nor
playing), is a **read-only** peak-level check each block (`FloatVectorOperations::findMinAndMax`
per channel, converted with `Decibels::gainToDecibels`) that just sets `autoRecordTriggered`
when the level crosses the threshold. `EditorToolbar`'s existing 15Hz timer -- already the one
place a knob move or transport-state change gets noticed -- is where the real start happens:
sees the flag, calls `processor.startRecording()` (message thread, same as a manual Record
click), clears the flag, un-arms the toggle. No new thread-safety category introduced, just the
same handoff pattern this class already uses everywhere else.

**UI.** New `autoRecordButton` -- placed right after Record at the user's explicit request
("Place it after the record button") -- toggles like Loop/Scrub (outlined off, filled-accent
armed), disabled while already recording. New `drawAutoRecordIcon`: three ascending level bars
crossed by a threshold line -- an icon of my own design this time (no reference image), meant to
read as "level meter + trigger line"; flagged to the user as a first pass, open to a different
treatment once they've seen it rendered. The threshold itself lives in a small `CallOutBox`
panel (`AutoRecordThresholdPanel`, modeled on the existing Amplify/Stretch panels but with no
Apply button -- it's a monitoring setting, not an audio edit, so the slider writes straight
through as it moves) reached via a new **Tools ▾ → "Auto-Record Threshold…"** item, consistent
with how Output Folder/Theme are already exposed there rather than inventing a new "Settings"
surface that doesn't otherwise exist in this app.

**Persistence.** `autoRecordThresholdDb` is saved in plugin state alongside the knob-row Speed/
Pitch/Stretch (state-format tag bumped `'R3W3'`→`'R3W4'`, same one-field-at-a-time bump pattern
every prior addition to this blob used) -- it's a setting worth remembering across sessions, not
a one-off UI toggle. `autoRecordEnabled`/`autoRecordTriggered` are deliberately **not**
persisted -- an "armed and waiting" state shouldn't survive a save/reload, the same reason
`isRecording`/`isPlaying` aren't persisted either.

Smoke test passes; all four targets build clean.

**Icon replaced with a gauge.** User: "can you use this for the threshold button?" -- supplied a
reference image (a circular gauge: ring, a shorter inner scale arc, a needle, a centre pivot
dot). Radial ray-cast it the same way as the gear/reel icons: the outer ring is a plain circle
throughout; the inner scale arc spans -95° to +5° (JUCE's 0=up/clockwise-positive convention) --
lower-left, up through the top, stopping just past it, not a tidy symmetric sweep; the needle
points to +45° (upper-right), well past the arc's own end, exactly as measured rather than
smoothed into pointing at the arc's tip. `drawAutoRecordIcon` (bars + threshold line) replaced
outright by `drawGaugeIcon`; the `iconAutoRecord` marker name is unchanged (still names the
button's function, same as `iconTools`/`iconScrub` keeping their marker names through multiple
icon redesigns). Smoke test passes; all four targets build clean.

## Revert to Original (replacing the old, buggy Revert)

User: "I'd like a option in settings to undo editing done to a loaded or recorded sample to
replace 'revert'. In the vst when I hit revert, it clears the recording. Now that we have a
clear recorder button 'revert' doesn't seem to be needed."

**Root cause of the bug being reported.** The old `revertAll()` was `while (canUndo()) undo();`
-- walk the undo stack all the way back. For a *loaded* file that's harmless (`loadFromFile()`
clears undo history right after loading, so there's nothing to walk back through). For a
*recorded* take it's destructive: `stopRecording()`'s own `commitChange(..., "Record")` is itself
just one more undoable step on the same stack, so walking all the way back undid the recording
itself, wiping it to nothing -- exactly the reported symptom, and exactly why Clear (which
already does "wipe to nothing" on purpose) makes the old Revert redundant at best and dangerous
at worst.

**The fix.** `AudioDocument` gained a separate snapshot, `originalBuffer`, untouched by ordinary
edits (Trim, Amplify, Paste, ...) -- `markAsOriginal()` re-takes it only at a genuinely new
starting point (`loadFromFile()`, `newEmptyDocument()`, `PluginProcessor::stopRecording()`,
`setStateInformation()`'s restore), and `revertToOriginal()` restores it as an ordinary
`commitChange()` -- ordinary in the sense that it's just as undoable/redoable as any other edit,
and it can never reach back further than the original take, because it doesn't touch the undo
stack's history at all, just adds one more edit that happens to restore an earlier snapshot.
Tools ▾'s "Revert" renamed to **"Revert to Original"**, enabled whenever there's anything loaded
(`! empty`) rather than the old `canUndo` (which was true/false for the wrong reasons -- see
above).

**A second, deeper bug found while testing the fix.** Writing a regression test for "reverting
is itself undoable" failed on the very first run: undoing a revert restored the *original* take,
not the edit that came before the revert. Root cause, found in JUCE's own `UndoManager` source
(`juce_UndoManager.cpp`): `perform()` only starts a genuinely new transaction right after
construction or a `clearUndoHistory()` call -- every `perform()` after that appends to whatever
transaction is already open, *forever*, unless something calls `beginNewTransaction()`. Nothing
in this codebase ever did. So every edit made since the last load/record/clear -- Trim, then
Amplify, then Fade In, however many -- was silently accumulating into a **single** undo step;
one Undo (⌘Z, or Tools ▾) would have reverted *all* of them at once, not just the last one, the
whole time this app has existed. Fixed with one line: `commitChange()` now calls
`undoManager.beginNewTransaction(actionName)` immediately before `perform()`, so every edit is
its own isolated, individually undoable/redoable step, matching what the Tools ▾ menu's
per-action Undo/Redo labels always implied. Added a dedicated regression test ("each edit is its
own undo step") independent of the Revert-to-Original one, since this affects every edit in the
app, not just the new feature that happened to surface it. Smoke test passes (both new sections);
all four targets build clean.

## Drag a sample in from Finder

User: "Have we worked on dragging and dropping samples into R3WRK? Doesn't seem to work" -- it
hadn't: there was drag-*out* (a selection to Finder/Ableton, `59302c7`) but nothing accepting a
drop coming *in*.

`R3WRKAudioProcessorEditor` now implements `juce::FileDragAndDropTarget` -- covering the whole
editor window, not just the waveform strip, since none of the child components implement the
interface themselves and JUCE's peer walks up the component hierarchy from whatever's under the
cursor until it finds one that does. `isInterestedInFileDrag`/`filesDropped` check the same
extension list Tools ▾ -> "Open…"'s `FileChooser` already filters to (`wav;aiff;aif;flac;ogg;
mp3`); a drop loads the first recognised file the same way Open does -- `EditorToolbar::openFile()`
had its FileChooser-success body factored out into a new public `loadAudioFile(file)` so both
paths share the exact same "header name, zoom-to-fit, saved/dirty state" bookkeeping instead of
duplicating it. `fileDragEnter`/`fileDragExit` toggle a thin accent-coloured border around the
whole window while a recognised file is being dragged over it, so there's some visual
acknowledgement before you let go. Smoke test passes; all four targets build clean -- **could
not verify the actual drag gesture end-to-end** (no mouse-drag-from-Finder scripting in this
environment), only that it builds, launches, and the extension-matching logic is correct by
inspection; flagged to the user to confirm the feel themselves.

## Trim added to the selection right-click menu + ⌘T

User: "Can you add trim to the drop down menu when you right click in a selection. Also add
command t for a shortcut." `WaveformDisplay::onSelectionContextMenu` only ever fires with a
selection already under the pointer (see its class doc), so "Trim to Selection" needed no enable/
disable check here, unlike Tools ▾'s copy of the same item (gated on `hasSelection()`, since that
menu is reachable with nothing selected). Added as the first item, ahead of Amplify/Reverse/
Stretch·Pitch, mirroring Tools ▾'s own ordering (Trim comes before those there too). New
`EditorToolbar::doTrim()` (`EditActions::trimToSelection(document)`, already a safe no-op with no
selection) wired to ⌘T in `PluginEditor::keyPressed`, alongside the existing ⌘X/C/V/Z transport
shortcuts. Tools ▾'s own "Trim to Selection" item now shows the same "⌘T" hint text Cut/Copy/
Paste already display next to theirs (cosmetic only -- the real key binding lives in
`keyPressed`, same as those). Smoke test passes; all four targets build clean.

## App icon

User: "Please use this as the app launch button" -- supplied a reference image (the same
notched reel-hub ring already traced for the Scrub button's icon, see `drawScrubIcon` /
`45771d0`'s predecessor). Rather than re-trace it, rendered the *exact same* geometry (outer
ring, six 60°-spaced notches, `ringInnerR = outerR*0.79`, `toothR = outerR*0.59`) at 1024×1024
in Python/Pillow, on a warm off-white rounded-square card matching the reference's own
presentation -- ties the app's launcher icon to its own Scrub button rather than introducing a
third, unrelated visual. Saved as `Plugin/Resources/AppIcon.png` (tracked in the repo, so the
icon regenerates identically on a clean checkout); wired via `ICON_BIG` on the `juce_add_plugin`
call in `CMakeLists.txt` -- JUCE's own build step converts it to an `.icns` and embeds it in
whichever bundle format can carry one (Standalone `.app`, VST3, AU), no manual `iconutil` step
needed. Adding `ICON_BIG` is a CMake *target property* change, not just a source change, so it
needed a full `cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .` reconfigure before
the next build would pick it up (a "just build" without reconfiguring silently keeps the old,
icon-less target definition). Verified by round-tripping the generated `.icns` back through
`iconutil -c iconset` and inspecting the extracted PNG -- pixel-identical to the source. Only
two sizes got embedded (512 and 512@2x/1024 -- JUCE's icon step doesn't synthesize the smaller
16/32/128/256 tiers some `.icns` files carry), which just means macOS downsamples from those for
small contexts (Finder list view, etc.) rather than using a purpose-drawn tiny version -- fine
for a hobby-project plugin, a known simplification if it's ever worth revisiting. Smoke test
passes; all four targets build clean. **Not independently visually confirmed in the Dock/Finder**
(no interactive way to inspect Finder's rendered icon in this environment) beyond the `.icns`
round-trip check -- flagged to the user to glance at Finder/the Dock themselves.

**Lighter background, per the user's very next message: "Can you make the background a little
more light? I can't see the vector."** The rounded-square white "card" (from the first pass
above) sat on a fully *transparent* canvas outside its own rounded corners -- reasonable if
something composites transparency correctly, but easy to end up looking mostly dark depending on
where it's rendered, which reads exactly like "the background is too dark, I can't see the
vector." Simplified rather than re-tuned: dropped the rounded-card-on-transparency approach
entirely and filled the *whole* 1024×1024 canvas with plain opaque white -- no transparency
anywhere -- since macOS already applies its own rounded-corner mask/shadow to a modern app icon,
so drawing one by hand wasn't buying anything, just risking exactly this kind of ambiguity.

**Gotcha hit re-verifying the fix:** overwriting `Plugin/Resources/AppIcon.png` in place and
re-running `cmake --build` alone did **not** pick up the new pixels -- the generated
`JuceLibraryCode/AppIcon.icns` (and its copies inside each bundle) kept their original file size
and an unchanged timestamp. `_juce_generate_icon` (JUCE's CMake icon step) only runs at
**configure** time, not as a per-build rule that re-checks its source file's content -- exactly
the same category of gotcha as changing `ICON_BIG` itself needing a reconfigure, just one layer
further in (this time the *path* didn't change, only the file's *content* did, and that's exactly
the case CMake's configure-time generation doesn't re-detect on a plain rebuild). Fix: delete the
stale generated `AppIcon.icns` copies under `build/` and reconfigure
(`cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .`) -- confirmed by the regenerated
`.icns`'s file size actually changing (23354 -> 19997 bytes) before rebuilding. **Rule of thumb
worth remembering: any edit to the icon source image needs a reconfigure, not just a rebuild, to
actually take effect** -- a plain rebuild will silently keep serving the stale icon. Re-verified
via the same `.icns` round-trip -- the extracted PNG now matches the plain-white version exactly.
Smoke test passes; all four targets build clean.

**Still "black on black" -- a second, deeper Dock cache, unrelated to any of the above.** User
sent a screenshot: the Dock tile really was rendering near-black, even though the `.icns` on disk
was already confirmed correct via the round-trip check and Finder's own "Get Info" preview panel
showed it correctly (white, ring clearly visible) -- so this was neither the icon file nor even
Finder's icon cache; it was specifically the **Dock's own on-disk icon cache**, a separate file
(`com.apple.dock.iconcache`, under the per-user `/private/var/folders/.../C/` cache directory)
that a plain `killall Dock` doesn't invalidate -- the Dock process just reloads the same stale
cache file from disk on relaunch. Fix: delete that file directly, then `killall Dock`. Confirmed
by temporarily disabling Dock auto-hide (`defaults write com.apple.dock autohide -bool false` +
`killall Dock`, restored after) to get a screenshot of the visible Dock -- the R3WRK tile now
shows the correct white background with the ring clearly legible. **Three separate caching layers
turned out to be involved across this whole icon saga**, each needing its own specific
invalidation: (1) Xcode/CMake's own build-vs-configure split for the source `.icns` generation
(needs a reconfigure, not just a rebuild, on any source-image edit -- see above); (2) Finder's
icon cache (a plain `killall Finder` sufficed); (3) the Dock's *separate*, persistent on-disk
icon cache (needed deleting the cache file itself, not just restarting the process). No source
changes this round -- purely an OS/build-cache issue, confirmed resolved by direct visual
inspection of the live Dock.

## Icon inverted: white ring + hub on black, "a tape player part"

User: "can you replace it with this? It looks more like a Tape player part" -- supplied a new
reference: the same reel-hub concept, but inverted (white on black) and constructed differently --
not a single filled ring-with-a-notched-hole like the previous version, but an **outer thin ring
(stroked circle) separated by a dark gap from a solid, filled 6-notch hub** sitting inside it --
closer to what an actual tape reel's rim + drive-hub look like photographed against black.
Ray-cast the reference the same way as every other traced icon this session: outer ring spans
`0.865x`-`1.0x` of the overall radius (a stroked annulus); the inner hub is solid from centre out
to `0.676x` between notches, cut inward to `0.541x` at each of the same 6 notches (60° spacing,
9° half-width) used throughout. Rebuilt `Plugin/Resources/AppIcon.png` as a plain black
1024×1024 canvas with a white annulus (two concentric filled circles, outer minus inner) for the
ring and a separately-filled notched-star polygon for the hub -- reconfigured (the icon-generation
gotcha from `1bd2777` applies to every source-image edit, not just the first one) and rebuilt.

**Confirming this one took three separate cache-clears, all three of the layers found so far were
already stale simultaneously**: deleted the Dock's on-disk icon cache (`com.apple.dock.iconcache`)
*proactively* this time (learned from the last round), but Finder's "reveal" preview panel *still*
showed the old icon after that plus a plain `killall Finder`. Root cause: a fourth layer below
both Dock's and Finder's own caches -- **`iconservicesagent`** (`/System/Library/CoreServices/
iconservicesagent`, a per-user background daemon; there's also a system-level `iconservicesd`
running as `_iconservices`, left alone since killing it needs root and wasn't necessary) with its
own persistent cache directories under `/private/var/folders/.../C/com.apple.iconservices*`. Fix:
`kill -9` the agent process, delete those two cache directories (no sudo needed, they're
per-user), `qlmanage -r` / `qlmanage -r cache` to reset the QuickLook daemon too for good measure,
then `killall Finder Dock`. That combination finally produced a correct, freshly-rendered preview.
**Updated rule of thumb: a stubborn stale-icon report needs clearing *all four* layers to be sure
-- reconfigure (source `.icns` generation), Finder cache, Dock's on-disk cache, and
`iconservicesagent`'s cache -- not just the first one or two that happen to fix it most of the
time.** No source changes beyond the new icon artwork; confirmed via a fresh Finder preview
screenshot matching the reference exactly. Smoke test passes; all four targets build clean.

## Recoloured the Standalone "audio input muted" banner

User: "Is there anyway you can change the color of the yellow bar with 'audio input is muted to
avoid feedback loop', or have a way to hide it?" That banner is JUCE's own stock UI, not R3WRK's
-- `NotificationArea`, a private nested class inside `juce_StandaloneFilterWindow.h`
(`juce_audio_plugin_client/Standalone/`), hardcoded to `Colours::lightgoldenrodyellow` fill /
`darkgoldenrod` border / black text, with no exposed customization hook (no virtual paint method,
no theme injection point) to reach from R3WRK's own source -- it only exists in the Standalone
build (the window wrapping the plugin editor), never in the VST3/AU, which don't have a
standalone audio device to worry about feeding back.

Chose recolour over hide: the muting itself is a real safety feature (stops live input feeding
back into output when they're the same device) worth keeping *visible* that it's active, just not
in JUCE's jarring stock yellow. Patched `NotificationArea`'s `paint()`/constructor directly in the
vendored `JUCE/` checkout to use R3WRK's own Midnight-theme colours (`panelBg` 0xff17191e fill,
`accent` 0xff5ec2ff bottom border, `screenText` 0xffe0e0e0 text, also applied to the "Settings…"
button) -- hardcoded rather than read live from `ThemeManager`, since this window lives *outside*
the plugin editor entirely (it wraps the editor, doesn't contain it) and has no reach into the
app's theme system without much deeper wiring than a cosmetic fix like this warrants.

**Made the edit durable across a fresh JUCE clone.** `JUCE/` is git-ignored and re-cloned fresh by
both `build.sh` and `BUILD_ON_MACOS.md`'s manual steps (`git clone --depth 1 --branch 8.0.15 ...`)
-- a hand-edit to the vendored checkout alone would silently vanish the next time someone (or a
future session) re-clones it. Captured the change as a plain `git diff` from inside the JUCE
checkout (it's its own git repo, so this was just `git diff > patch-file`), saved as new
`patches/juce-standalone-notification-bar.patch` (+ `patches/README.md` explaining the mechanism
and why it's hardcoded rather than theme-aware) -- `build.sh` now applies every `patches/*.patch`
automatically right after a fresh clone; `BUILD_ON_MACOS.md`'s manual-clone snippet got the same
loop added inline for anyone not using the script. Smoke test passes; all four targets build
clean (only the Standalone target actually relinks against the changed header -- VST3/AU are
unaffected, as expected, since they never build that file at all).

## Rounded corners on the Standalone window

User: "Can you round the corners on the desktop app?" Checked first rather than assuming: a
zoomed screenshot of the actual running window showed a perfectly sharp 90° corner -- no rounding
at all, not even the subtle system-standard radius most native macOS windows get automatically.
Root cause: `StandaloneFilterWindow` (JUCE's Standalone wrapper, via `DocumentWindow`) uses a
**non-native title bar** (the plain "Options" pill button seen in every screenshot this session,
not real traffic-light buttons) -- confirmed in JUCE's own source
(`TopLevelWindow::getDesktopWindowStyleFlags()`) that `ComponentPeer::windowHasTitleBar` is only
added when `useNativeTitleBar` is true, so this window's native peer never gets the standard
titled-window treatment macOS auto-rounds; effectively a borderless-style window, which macOS
does not round on its own.

**No cross-platform JUCE feature reaches this** -- no "rounded window" setting, no exposed hook
to the native NSWindow/CALayer from a `Component`. This needed real native code: new
`Plugin/Source/StandaloneWindowShape.h`/`.mm` (`#if JUCE_MAC`-guarded, harmless no-op include on
other platforms since it's simply not called there) clips the window's content view to a
rounded-rect `CALayer` mask and makes the underlying `NSWindow` non-opaque with a clear
background, so the four corners the mask crops away read as genuinely transparent (desktop
showing through) rather than a mismatched solid square. Deliberately done via the layer mask
(clips *everything* JUCE paints, any nesting depth) rather than overriding `paint()` and calling
`Graphics::reduceClipRegion()` to a rounded path -- traced through JUCE's own
`Component::paintComponentAndChildren()` and confirmed a `Graphics::ScopedSaveState` restores the
clip **before** children are painted, so a clip set inside one Component's own `paint()` never
reaches its children; the actual editor content filling the rest of the window would have kept
its square corners peeking out past a window-level-only clip.

One call wired into JUCE's own `StandaloneFilterWindow` constructor (`patches/juce-standalone-
rounded-corners.patch`, same durability mechanism as the notification-bar patch -- captured as an
isolated `git diff` from inside the JUCE checkout, `build.sh` already applies every
`patches/*.patch` automatically). **Two real compile issues surfaced getting there, both fixed,
both worth remembering:**

1. **Cocoa/JUCE include-order collision.** `#import`ing `<Cocoa/Cocoa.h>` *after* JUCE headers
   makes Carbon's `Components.h` (pulled in transitively) declare a plain global `Component`
   typedef that collides with `juce::Component` once JUCE's own `using namespace juce;` has
   already brought that name into unqualified scope -- "reference to 'Component' is ambiguous".
   Fix: import Cocoa *first*, before any JUCE include, in the `.mm` file -- the same ordering
   JUCE's own native `.mm` files always use.
2. **An ambient, unclosed nested `namespace juce {}` inside JUCE's own amalgamated headers.**
   The forward declaration this patch adds to `juce_StandaloneFilterWindow.h` sits (via
   `juce_audio_plugin_client_Standalone.cpp`'s particular include order) inside a `namespace juce
   { ... }` opened earlier by a different module header and not yet closed -- so *any* spelling
   of the qualifier, including the root-relative `::juce::Component`, resolved as "look for a
   nested namespace called juce inside the juce namespace we're already in" and failed to
   compile. Fixed by sidestepping the type resolution entirely at the JUCE-patch call site: the
   forward declaration and the constructor's call both use `extern "C" void
   r3wrkApplyRoundedWindowCorners(void* window, float cornerRadiusPx)` -- a bare `void*` needs no
   namespace lookup at all -- and only the `.mm` implementation (a context with no such ambient
   nesting) casts it back to `juce::Component*`.

Verified by zooming into a fresh screenshot of the actual corner -- clearly rounded, desktop
visible through the corner, no artefacts. 12px radius, matching roughly what modern macOS's own
standard titled windows use. Smoke test passes; all four targets build clean.

## Slice markers + Slice / Octatrack export

> **The right-click model below was replaced by a Slice *tool* one message later -- see "Slice
> tool (redesign)" further down. The data model, `.ot` writer and export functions carried over
> unchanged; only the waveform interaction and the marker visual changed.**

User's plan: right-click the waveform to drop slice markers, then two Tools ▾ actions -- slice
each region to its own file in a folder, and export the slices as an Octatrack ".ot" file for
their hardware sampler.

**Data model (`AudioDocument`).** `std::vector<int64_t> sliceMarkers` -- message-thread only
(there's no slice *playback*, the audio thread never touches them), kept sorted +
de-duplicated + strictly inside `(0, getNumSamples())` by `normaliseSliceMarkers()`, which runs
after every add. `getSliceRegions()` turns k markers into the k+1 half-open ranges between `0`,
the markers, and the clip end (empty when there are no markers). ~~Persisted in plugin state
(state tag bumped `'R3W4'` -> `'R3W5'`), written *after* the variable-length audio so an older
blob without them still loads.~~ **Superseded a few messages later** -- markers are now
*session-only*, never written to or read from plugin state (see the "Slice tool (redesign)"
section); the `'R3W5'` tag stayed. **Deliberately not in the undo snapshot** -- and any edit that
changes the sample length (trim/cut/paste/stretch/...) clears them, since their absolute
positions would no longer line up with the audio (see `restoreSnapshot()` comparing old vs new
length). Non-length-changing edits (amplify/normalize/fade/reverse/silence) keep them. `Clear`
and load/record clear them too.

**Right-click UI (`WaveformDisplay`).** The popup-menu `mouseDown` path now branches: inside a
selection body -> the existing Amplify/Reverse/Stretch menu (unchanged); anywhere else ->
`showSliceMarkerMenu()`, a small inline `PopupMenu` -- "Add slice marker" (or "Delete slice
marker" when the click lands within ~6 screen px of one, via `findSliceMarkerNear()` in
sample-space) plus "Clear all slice markers" when any exist. Markers draw in `paint()` as a
1px accent line with a small downward triangle flag at the top edge, so they read as markers
rather than selection brackets, and clip out cleanly when scrolled off-view.

**`.ot` writer (`OctatrackOtFile.{h,cpp}`, kept free of AudioDocument/GUI types so the headless
SmokeTest exercises it directly).** Matches the layout the open-source **OctaChainer** tool
writes (which the user has installed and which the Octatrack loads): exactly **832 bytes**,
every multi-byte integer **big-endian** --
`FORM\0\0\0\0DPS1SMPA` magic + 7 reserved bytes `00 00 00 00 00 02 00`, then tempo (BPM*24),
`trim_len`/`loop_len` (= whole-clip sample count), stretch/loop mode (both off), gain `0x30` (0
dB), trig quantize `0xFF`, `trim_start` 0 / `trim_end` = sample count / `loop_point` 0, then
**64 fixed slice slots** of `{ uint32 start, uint32 end, uint32 loopPoint }` (used slots get
`loopPoint = 0xFFFFFFFF` "no loop", unused slots all-zero), then `uint32 slice_count`, then a
`uint16` checksum = `(sum of every byte from offset 0x10 to 0x33D) & 0xFFFF`. The slice
start/end offsets are exact sample positions; `bpm` (default 120, hardcoded for now -- R3WRK
tracks no BPM) only feeds the tempo field. **Flagged to the user as needing a hardware check:**
`trim_len`/`loop_len` units and the per-slice loop-point sentinel are the fields carried over
from OctaChainer's convention that I'm least certain of -- everything structural (size, magic,
offsets, checksum, slice offsets, slice count) is covered by a real SmokeTest section that
asserts the byte layout.

**Export (`EditActions` + Tools ▾).** `sliceToFolder(doc, folder, baseName)` writes each region
as `"<baseName> NN.wav"` (24-bit, zero-padded index) into a `"<timestamp> slices"` subfolder of
the output folder; `exportOctatrackChain(doc, wavFile, bpm)` writes `<name>.wav` (whole clip)
plus a sibling `<name>.ot`, falling back to a single whole-clip slice if there are no markers
and capping at the Octatrack's 64-slice limit (the toolbar reports the count and whether it was
capped via the header status line). Tools ▾ gains "Slice to Folder…" (enabled only with markers
present) and "Export Octatrack Chain (.wav + .ot)…" (enabled with any audio).

**Not interactively verified end-to-end** -- no way to script a right-click / menu-pick / mark
placement in this environment; the marker maths, region computation, slice-file output and the
full `.ot` byte layout (including the checksum) are covered by the new SmokeTest section, and
the app builds + launches clean, but the actual right-click feel and the .ot loading on the
Octatrack itself are for the user to confirm. Smoke test passes; all four targets build clean.

## Slice tool (redesign)

User, right after the above: "I'd like a button that puts R3WRK in slice marker mode. Double
click adds a slice marker, a click after or before plays that slice, a left click on the slice
marker deletes the slice, clicking and dragging moves the slice marker. I'd also like the slice
markers to be similar size to the selection bars."

**New `sliceButton`** (`R3WRKLookAndFeel::iconSlice` -- originally a little four-bar waveform
with a cut line; **replaced at the user's request with a box-cutter / utility-knife line icon**
matching a reference PNG they supplied: `drawSliceIcon` strokes a capsule handle with an inner
U-slot, a thumb-slider tab, and a slim chamfered blade with one snap-line, built in a local
frame and leaned ~4deg CCW, then screenshot-tuned for centring the same way as the gear/reel
icons), sits right after Scrub in the toolbar, same toggling-tool styling as Loop/Scrub (outlined off,
accent-filled on), disabled while recording. `AudioDocument::sliceModeEnabled` (plain bool,
message-thread only -- the audio thread still never touches slices; "play slice" just sets the
selection + normal playback). **Mutually exclusive with Scrub** -- turning either on turns the
other off (both `document.*ModeEnabled` and the button toggle state).

**`WaveformDisplay` repurposes the mouse while `sliceModeEnabled`** (gated at the very top of
each handler, ahead of the scrub check and the selection state machine):
  - `mouseDown` records `sliceDragIndex` = the marker within `sliceHitTolerance()` (~6 screen px
    in samples, measured mid-view so `xToSample()`'s edge clamp doesn't skew it) of the press,
    or -1;
  - `mouseDrag` on a marker calls `AudioDocument::moveSliceMarker(index, xToSample(x))`, which
    repositions + re-normalises and returns the marker's new index (drag keeps tracking it; if
    dragged exactly onto another marker the two collapse and it tracks the survivor);
  - `mouseUp`: a clean click (`getDistanceFromDragStart() < 4`, no drag) that did *not* land on
    a marker calls `playSliceAt()` -- sets the selection to the clicked region, drops the
    playhead at its start, and fires the new `onSlicePlay` callback (`PluginEditor` ->
    `processor.startPlayback()`), which plays the region once; with no markers it just plays
    from the click point. A clean click *on* a marker line does nothing (drag it, or
    double-click its handle to delete);
  - `mouseDoubleClick` (**handle-only delete**, per the user's follow-up: "deleting a slice
    marker should only be done when double clicking the top and bottom of the marker, where
    there is thicker chunks"): a double-click within `sliceHandleZonePx` (20px) of the top or
    bottom edge *and* on a marker deletes that marker; a double-click that's not on any marker
    adds one; a double-click on the thin middle of a marker line does nothing. This handler is
    **self-contained** -- it re-runs the marker hit-test + handle-zone check from the event
    rather than trusting state from `mouseDown`, because JUCE dispatches `mouseUp` (which
    clears `sliceDragIndex` / `slicePressOnMarker`) *before* `mouseDoubleClick`
    (`juce_Component.cpp` ~L2586 vs ~L2603);
  - `mouseMove` shows a left/right-resize cursor over a marker, a crosshair elsewhere.

**Known rough edge (accepted for v1, flagged to the user):** a double-click's *first* click,
when it's not on a marker, runs the "play slice" path before `mouseDoubleClick` adds the
marker, so a marker-add plays a blip of that slice. Fixing it cleanly needs a debounce timer
(defer the play past the double-click window); left out unless it turns out to bother -- for an
audition-while-you-slice workflow, a blip of the slice you're splitting is arguably fine
feedback. (Deleting via a handle double-click has no such blip -- the click lands on a marker,
so the play path is suppressed.)

**Markers redrawn** to match the selection brackets' size (the "similar to the selection bars"
ask): the same 2px vertical line + 5x14 rounded handle pill at top and bottom, but in
`pal.loopMarker` (the orange loop/unsaved colour) instead of the selection's accent-blue, so the
two don't read as the same thing when a slice-body click leaves a selection showing.

**The old right-click menu is gone** -- `showSliceMarkerMenu()` removed, the popup-menu
`mouseDown` branch reverted to only the selection-body Amplify/Reverse/Stretch menu it had
before. "Clear all" moved to a new Tools ▾ -> "Clear Slice Markers" item (the Slice-to-Folder /
Export Octatrack Chain items are unchanged). New SmokeTest coverage for `moveSliceMarker`
(reposition / reorder past a neighbour / merge onto another).

**Markers only exist while the Slice tool is on** -- `WaveformDisplay::paint()` draws them
inside an `if (document.sliceModeEnabled)` block, so turning the button off hides every marker
and (the mouse handlers were already gated) hands the waveform straight back to normal editing.
The markers themselves stay in `AudioDocument::sliceMarkers` and come back the instant the tool
is re-enabled.

**Markers are session-only, never persisted** (user: "save for that session, not in permanent
memory"). `PluginProcessor::get/setStateInformation` no longer write or read the marker list --
they live purely in `AudioDocument` for the life of the plugin instance, gone on reload / DAW
project save / standalone relaunch. `kStateMagic` stays `'R3W5'` (not reverted) so a state blob
already written with a trailing marker list still loads -- `setStateInformation` just stops
reading after the audio and ignores the extra bytes.

**Not interactively verified** -- the button toggles, marker draw and mutual exclusion are
visible in a screenshot, but the actual double-click / drag / click-to-play feel -- including
the handle-only delete -- is the user's to try, same as the export-to-hardware step. Smoke test
passes; all four targets build clean.

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
