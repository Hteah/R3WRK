# Wave Scanning Plan

How R3WRK moves a playing loop across a sample (wave scanning) without crackles: by hand, and
from an Ableton LFO, automation or a MIDI CC. Written 2026-10-02, after the LFO case finally
worked cleanly.

## The problem

A looping selection is being moved while it plays. Each time the loop window changes, playback
has to go somewhere. Doing that naively is a discontinuity, and you hear it as a click. Do it
dozens of times a second and you hear crackle, scratching or buzz.

There were two different ways the window moves, and each needed its own answer:

| Source | How the window moves | Earlier symptom |
|---|---|---|
| **Hand drag** (Start / End knob, waveform brackets) | small steps at mouse rate, roughly where you're looking | click storm, pitch rise, clicks on release |
| **Remote** (Live LFO / Macro / automation, MIDI CC) | continuous, often far faster than playback (an LFO across a 24-minute file asks for hundreds of x) | crackle, then "plays from one spot", then clicks again |

## The pieces (where they live)

- **`Plugin/Source/DragScanRender.h`**: the renderer. It reads at a constant 1x through a window
  whose edges move per sample. When the window passes the playhead, the playhead **relocates**
  to the same point in the loop's cycle with a 10 ms equal-power crossfade, instead of racing to
  catch up (racing made the pitch rise). `renderReleaseTail` blends out what it would have played
  next when a drag ends.
- **`PluginProcessor::processBlock`**: decides when the renderer is used and how fast the window
  may move (the "slew"):
  - a **hand drag** glides at 8/s, capped just under the playhead's speed
    (`dragscan::maxWindowSpeed`), tuned by ear;
  - a **remote move** follows closely, about 30 ms, with **no speed cap**.
- **`AudioDocument::selectionEdgeDragging`**: set by the knobs and brackets while you drag
  (WaveformDisplay self-heals it if a mouse-up is lost).
- **`AudioDocument::selectionRemoteMoving`**: set by the processor's 60 Hz timer while a host
  parameter or CC moves Start / End / Position, and released 250 ms after the last move.
  `processBlock` treats it like a drag, flagged as remote (`dragIsRemote`).
- **`dragscan::renderBlock(..., remote)`** is the remote-mode switch (see below).
- **Host parameters / MIDI** (`HostParams.h`, `KnobBinding.h`, `MidiCcDispatcher.h`):
  - **Position** (CC 28) slides the whole selection and keeps its length.
  - Start and End move only their own edge.
  - Only the knob actually moved gets reported back to Live (`keepOnlyTheMovedSelectionChange`).

## What made the hand drag work (2026-09-26)

1. Modelled the render path with a constant 1.0 signal, so any gain step is visible no matter
   what the material is.
2. Traced every click to a branch and sample index before fixing anything. All of them were
   region edges teleporting once per block.
3. Moved the renderer into a header (`DragScanRender.h`) so the smoke test drives the real code.
   Every test also checks that the **old behaviour fails**, which proves the test can actually
   see the bug.
4. Read at 1x and relocate with a crossfade instead of catching up (this fixed the pitch rise),
   and cap the window just under the read speed.
5. Added a release tail (fixed the click on about one release in three).

## What made the LFO / remote case work (2026-10-02)

Each step was found by measuring, not guessing:

1. **Crackle.** The drag-scan path only engaged for a hand drag. An LFO moved the selection with
   the flag off, so the loop teleported about 60 times a second.
   Fix: `selectionRemoteMoving`, so remote moves use the drag-scan path too.
2. **"Plays straight through".** Under the hand-drag speed cap (<1x), the window could never
   catch an LFO. The playhead rode along inside it, and you heard the file playing through.
   Fix: remote moves follow closely, first capped at 8x.
3. **"Plays from one spot"** (from the screen recording): the file was **24 minutes** long, so
   even 8x left the window crawling a few seconds while the LFO swept the whole file.
   Fix: no speed cap for remote moves. That brought the clicks back: 62 at a 0.11 Hz full sweep,
   170 at 0.5 Hz.
4. **One guess, measured and reverted.** I tried "don't start a relocation mid-crossfade" on its
   own. It changed nothing, so I reverted it instead of stacking fixes.
5. **The trace that cracked it.** An offline, sample-by-sample run of the real renderer, with the
   same 60 Hz target steps the plugin sees, labelled every click:
   - **73 of 91:** the loop's **edge fade**, measured against a window edge racing past the
     playhead at about 100x. It collapsed from 1 to 0 in about 4 samples, which is a hard cut,
     not a fade.
   - **18 of 91:** a new relocation starting while the previous crossfade was still running.
     This dropped the half-faded voice.
6. **Remote mode in the renderer** (`renderBlock(..., remote = true)`). Hand drags keep the
   tuned path untouched.
   - no edge fade against a racing edge;
   - the loop seam is a 10 ms relocation crossfade (the old side plays on past the end, like a
     tape-loop splice);
   - relocations never overlap.

Result, real VST3:

| Test | Before | After |
|---|---|---|
| 10-minute file, LFO 0.11 Hz full sweep | 62 clicks | **0** |
| 10-minute file, LFO 0.5 Hz full sweep | 170 clicks | **0** |
| Short file, LFO up to 8 Hz; Start / End LFOs | some | **0** |
| Level dips | some | **none** |

Playback follows the sweep end to end.

## Start knob (hand) + the echo bug (2026-10-02, later)

- The on-screen **Start** knob slides the loop (End follows, length kept) by hand: that's the feel
  the user wants. An edge-only Start and an extra POS knob were tried and rejected. Position
  stays the parameter for LFO / MIDI scanning (an LFO or CC on Start trims the edge).
- **Bug found after restoring it:** sliding Start widened the loop when moving right and shrank it
  when moving left. Cause: reporting the knob move to the host (`syncToHost` ->
  `setValueNotifyingHost`) also called our own `setValue`. For Start / End that parks the value,
  and the next timer tick re-applied it as an edge-only Start, 16 ms stale. `setValue` now ignores
  the call made from inside our own report. SmokeTest reproduces it (40000 -> 45000 samples
  before the fix, 40000 after). User: works manually and with the LFO on Position, "sounds
  great". Merged to `main` 2026-10-02.

## How it's tested

- **SmokeTest** (`Plugin/Tests/SmokeTest.cpp`): the `[DragScan]` blocks drive the real renderer.
  `[DragScan] remote (LFO) sweep over a long file`:
  - the hand-drag path must click there (contrast, 232);
  - remote mode must not (0);
  - playback must follow the LFO across the file.
- **Real-plugin harness** (scratch, `vst3probe/`). It loads the installed VST3 like a DAW,
  records into it with MIDI CC Record, loops, and drives a parameter in real time the way
  Live's LFO does:
  - `LFOTest`: records a 110 Hz sine and counts second-difference spikes, which are clicks.
    Settings come from `REC_SECS`, `SEL` and `XF0`.
  - `LFOTrace`: records a 0..1 ramp, so the output value *is* the read position, and prints
    where playback actually reads compared with the window. With `DC=1` it records a constant
    instead and counts level dips (gaps).
  - zsh gotcha: pass the arguments separately; `$a` in a for-loop doesn't word-split.

## Trade-offs (known, accepted)

- **Remote moves with a fast LFO sound choppy or grainy.** The playhead keeps relocating with
  10 ms crossfades. That's what makes it click-free. Slow LFOs add nothing.
- **Plain loop only while moving.** During any drag or remote move, the loop plays as a plain
  forward loop: ping-pong and reverse come back when it stops.
- **Hand drags still glide** under 1x, so long hand moves arrive a little late. This is tuned by
  ear and unchanged.

## If it regresses

Follow the same loop:
1. reproduce with the harness;
2. trace clicks per sample and per branch;
3. fix only what the trace names;
4. add a smoke test whose old-behaviour contrast fails;
5. revert any guess that measures no better.
