# R3WRK — project notes

An Edison-style pop-out audio recorder/editor, built as a JUCE plugin
(VST3 + AU + standalone app). Full-featured v1: waveform + spectrogram views,
record/play/loop, cut/copy/paste/trim/delete/undo, normalize/gain/fade/reverse/
silence, RubberBand-powered time-stretch & pitch-shift, and BPM-based
chop-to-grid with per-slice WAV export.

Compiled and passing on Linux (VST3 + Standalone); see `BUILD_ON_MACOS.md` for
producing a native macOS VST3/AU with Xcode.

## Layout

```
edison-clone/
  JUCE/                    JUCE framework (cloned separately, see BUILD_ON_MACOS.md)
  Plugin/
    CMakeLists.txt
    Source/
      AudioDocument.h/.cpp     the document: buffer, selection, playhead, loop,
                                chop markers, undo history (snapshot-based)
      EditActions.h/.cpp       cut/copy/paste/trim/delete/normalize/gain/
                                fade/reverse/silence/export-slices, all
                                operating on AudioDocument as single undo steps
      TimeStretchEngine.h/.cpp offline time-stretch/pitch-shift via librubberband
      WaveformDisplay.h/.cpp   waveform view: draw, select, zoom, scroll
      SpectrogramDisplay.h/.cpp FFT spectrogram view of the whole document
      EditorToolbar.h/.cpp     every button/slider; owns the clipboard, talks
                                directly to AudioDocument/EditActions
      PluginProcessor.h/.cpp   audio thread: record/playback/pass-through,
                                plugin state save/load
      PluginEditor.h/.cpp      wires EditorToolbar + WaveformDisplay +
                                SpectrogramDisplay together
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
trim/delete/insert-silence, gain/normalize/fade/reverse/silence, chop-marker
math + slice export, RubberBand time-stretch/pitch-shift at several ratios,
the exact `replaceRangeWith` path the "Apply Stretch/Pitch" button uses, and
a save-to-WAV / load-from-WAV round trip including resample-on-load. All of
this passed as of the last build in this container.

## How editing/undo works

Edits are snapshot-based rather than diff-based: `AudioDocument::beginChange()`
copies the current buffer, the caller builds a new buffer (via `EditActions`
helpers, which never touch the live buffer directly), and
`AudioDocument::commitChange(newBuffer, name)` pushes a `juce::UndoableAction`
that swaps between the two full copies. This was the deliberate simplicity
trade-off for a v1: it's easy to reason about and hard to get subtly wrong,
at the cost of memory scaling with (audio length) × (undo steps kept). For
typical Edison-style use — chopping/editing individual samples/loops rather
than hour-long recordings — that's a fine trade.

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

## Sample-rate handling

The document stores audio at whatever rate it was recorded or loaded at.
Loading a file resamples it (via `juce::LagrangeInterpolator`) to the host's
current sample rate if they differ, so pitch/speed is correct in your DAW.

## Known gaps / natural next steps

- Recording is destructive-replace only (no overdub/punch-in/multiple takes).
- The spectrogram recomputes for the *whole* document on any audio-content
  change; fine for typical sample lengths, would want to be a smarter
  incremental/windowed computation for very long recordings.
- No keyboard shortcuts yet (space to play/stop, etc.) — everything is
  button/mouse driven.
- The time-stretch/pitch-shift preview is "commit only" (no live audition
  before applying) — you set the sliders and hit Apply.
- No AAX/LV2 build target configured (VST3 + AU + Standalone only), since
  those weren't asked for; JUCE supports adding them if you want them later.
