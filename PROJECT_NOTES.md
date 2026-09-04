# R3WRK — project notes

An Edison-style pop-out audio recorder/editor, built as a JUCE plugin
(VST3 + AU + standalone app): waveform + spectrogram views, record/play/loop,
cut/copy/paste/trim/delete/undo, normalize/gain/fade/reverse/silence,
RubberBand-powered time-stretch & pitch-shift, export-selection to WAV, and a
live knob row (tape Speed + Pitch that re-pitch playback in real time; Start/End
that drive the selection edges).

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
      EditorToolbar.h/.cpp     transport strip + Tools menu; owns the clipboard,
                                talks directly to AudioDocument/EditActions
      KnobRow.h/.cpp           rotary knob strip under the transport bar
                                (Pitch, Speed, Start, End; extensible)
      PluginProcessor.h/.cpp   audio thread: record / playback (direct or via a
                                real-time RubberBand tape+pitch engine) / pass-
                                through, plugin state save/load
      PluginEditor.h/.cpp      wires HeaderBar + WaveformDisplay/SpectrogramDisplay
                                + EditorToolbar + KnobRow together
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

## Recording model

Recording captures into a separate growable accumulator buffer on the audio
thread (cheap, allocation-free after the first grow) and is only merged into
the document (as a normal undoable "Record" edit) when you stop. Recording
currently always replaces the whole document — there's no overdub/punch-in
yet.

## Playback & the knob row

`processBlock` plays the document region (the selection if any, else the loop
points, else the whole clip) straight from the buffer under a try-lock. The
`KnobRow` under the transport bar adds:

- **Speed** — tape rate. 1.0 = normal; 2.0 plays twice as fast *and* an octave
  up, pitch riding along exactly like tape.
- **Pitch** — an extra ± semitone shift layered on top of the tape speed, so you
  can detune without changing the playback rate.
- **Start / End** — normalised (0–1) knobs that set the document selection edges;
  they read out as `m:ss.mmm` and follow bracket drags on the waveform.

Speed and Pitch are `std::atomic<double>` on the processor. When *both* are
centred (`speed==1`, `pitch==0`) playback is a plain sample copy — zero latency,
zero cost. As soon as either is off-centre, playback routes through a **real-time
RubberBand stretcher** (`OptionProcessRealTime | OptionPitchHighConsistency`),
built per `prepareToPlay`: `timeRatio = 1/speed`, `pitchScale =
speed * 2^(pitch/12)`. The stretcher is fed doc-region samples in
`getSamplesRequired()`-sized blocks from preallocated scratch buffers and drained
via `available()`/`retrieve()`; it's `reset()` at the start of each play pass and
whenever the knobs cross the bypass/engaged line. The stored audio is never
modified — Export Selection still writes the dry clip. The red playhead tracks
the doc read cursor, so it runs slightly ahead of what you hear by the stretcher
latency when the knobs are engaged.

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
