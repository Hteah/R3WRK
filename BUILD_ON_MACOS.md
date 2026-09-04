# Building R3WRK on macOS (VST3 + AU + Standalone)

Confirmed building and running on macOS (Apple Silicon, Xcode 26, macOS 26 SDK)
with **JUCE 8.0.15**. The headless engine test suite (`Plugin/Tests/SmokeTest.cpp`)
passes.

## 1. Prerequisites

- **Xcode** (full app, from the App Store — command-line tools alone can't build
  AU/VST3 bundles). Then `xcode-select --install` for the CLI tools.
- **CMake** ≥ 3.22. No Homebrew needed — grab the official build:

  ```bash
  # pick the current version from https://github.com/Kitware/CMake/releases
  curl -fsSL -o /tmp/cmake.tar.gz \
    https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-macos-universal.tar.gz
  mkdir -p ~/.local/bin && tar xzf /tmp/cmake.tar.gz -C /tmp
  ln -sf /tmp/cmake-4.4.3-macos-universal/CMake.app/Contents/bin/cmake ~/.local/bin/cmake
  export PATH="$HOME/.local/bin:$PATH"
  ```

**RubberBand is *not* a prerequisite.** It has no CMake build (it's Meson), so the
`Plugin/CMakeLists.txt` fetches its source and compiles the shipped single-file unit
(`single/RubberBandSingle.cpp` — vDSP FFT + BQ resampler on Apple, no threads) into a
static lib. If a system RubberBand ever *is* found via `pkg-config`, that's used
instead.

## 2. Get the code

```bash
git clone <this repo> R3WRK && cd R3WRK
git clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git
```

`JUCE/` must sit next to `Plugin/` (the `CMakeLists.txt` does `add_subdirectory(../JUCE)`).
It's git-ignored.

## 3. Configure and build

```bash
cd Plugin
cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .
cmake --build build --config Release --target R3WRK_Standalone R3WRK_VST3 R3WRK_AU
```

Or run `./build.sh` from the repo root (clones JUCE if missing, then does the above).

The headless engine test:

```bash
cmake --build build --config Release --target R3WRKSmokeTest
./build/R3WRKSmokeTest_artefacts/Release/R3WRKSmokeTest
```

## 4. Where the artefacts land

```
Plugin/build/R3WRK_artefacts/Release/Standalone/R3WRK.app
Plugin/build/R3WRK_artefacts/Release/VST3/R3WRK.vst3
Plugin/build/R3WRK_artefacts/Release/AU/R3WRK.component
```

## 5. Install into the plugin folders

```bash
cd Plugin/build/R3WRK_artefacts/Release
codesign --force --deep -s - Standalone/R3WRK.app VST3/R3WRK.vst3 AU/R3WRK.component
cp -R VST3/R3WRK.vst3      ~/Library/Audio/Plug-Ins/VST3/
cp -R AU/R3WRK.component   ~/Library/Audio/Plug-Ins/Components/
```

Ad-hoc signing (`-s -`) is enough for a locally-built plugin to load; no paid Apple
Developer account required. Then rescan plugins in your DAW.

## 6. What to try first

1. Open **R3WRK.app** (Standalone) — fastest way to test without a DAW.
2. **Record** (mic), **Stop Rec** (same button).
3. **Trim**, **Normalize**, **Reverse**; drag a selection, move the Stretch/Pitch
   sliders, **Apply Stretch/Pitch**.
4. **Waveform** / **Spectrogram** toggle.
5. Set a BPM + division, **Update Grid**, **Export Slices…**.
6. Load `R3WRK.vst3` / `.component` in a DAW as a track insert.

See `PROJECT_NOTES.md` for architecture and known v1 limitations.
