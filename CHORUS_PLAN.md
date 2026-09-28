# CHORUS_PLAN.md — r3wrk CHORUS (smooth → metallic)

Research + implementation plan for a new FX-drawer effect in R3WRK. Written to be handed to
Claude Code as-is. Repo: `Hteah/R3WRK` (plan written against commit `a92680c`, "Add RTRG buffer
retrig; RVB and PLX share one drawer slot").

**Goal:** one chorus that covers two sounds. At one end it's the classic, lush, hissy 80s
poly-synth BBD chorus (the Roland Juno-60/106 three-button chorus). At the other end it's a
short, resonant, ringing *metallic* comb/flanger tone. One pill, two drawer knobs, a popup for
the rest.

**Decisions already made by Heath:**
- Controls: Juno-style **MODE** (I / II / I+II) for the smooth side, plus one **METAL** knob
  that shortens the delay and adds feedback for the ringing side.
- Chain position: **right after the filter**.
- Process: Claude Code builds from this plan. Commit this file to the repo root first, then go
  engine → SmokeTest → wiring → panel (§7).

---

## 0. Read these first (Claude Code)

1. `CLAUDE.md`, especially **"Conventions worth knowing before extending this codebase"**. The
   ones that matter here:
   - One plain `namespace r3wrk` struct per effect: header-only, `noexcept`, plain doubles, no
     allocation after `prepare()`, **no base class, no virtual dispatch**.
   - **New DSP gets an offline `Tests/SmokeTest.cpp` assertion before it goes live.** This is a
     project rule.
   - Not `juce::dsp`. The engine has to compile into the headless `R3WRKSmokeTest` target.
   - Every `setLookAndFeel(&x)` needs a matching `setLookAndFeel(nullptr)` in the destructor.
   - State persistence uses the incrementing magic constant plus a cascading `hasX` chain.
     Never reorder or remove an existing read.
2. Templates to copy:
   - **`Source/RetrigEngine.h` + `Source/RetrigPanel.h`**: the newest effect. It's header-only
     (engine and panel), sits right after the filter, and has the exact drawer layout
     (pill + 2 knobs + "more" dot) and popup pattern (`PinnableCallout` + `lcd::HardwareLcdLookAndFeel`).
   - **`PluginProcessor::applyMimeophon()`**: how the enable-edge reset works (reset on the
     `engaged && !lastEngaged` edge only, **not** on `freshPlayPass`).
   - **`Source/DirtStage.h`**: per-sample parameter ramping inside the engine, a bit-exact bypass
     when off, and self-healing if state goes non-finite.
3. Shared primitives already in `Source/ReverbEngine.h`: `DelayLine` (with `readInterpolated`),
   `OnePoleLowpass`. You can reuse them. For the chorus, prefer adding a **4-point Hermite read**
   to the chorus's own delay line (see §4.4) rather than editing `DelayLine`, per the
   "don't touch proven code" rule.

---

## 1. Research: the Juno chorus

### 1.1 Measured values (Juno-60)

From Andy Harman's measurements of a real Juno-60 (`pendragon-andyh/Juno60`, Chorus README):

| Mode | LFO rate | LFO shape | Delay min | Delay max | Centre ± depth |
|---|---|---|---|---|---|
| **I** | 0.513 Hz | triangle | 1.66 ms | 5.35 ms | 3.505 ± 1.845 ms |
| **II** | 0.863 Hz | triangle | 1.66 ms | 5.35 ms | 3.505 ± 1.845 ms |
| **I+II** | 9.75 Hz | triangle | 3.3 ms | 3.7 ms | 3.5 ± 0.2 ms |

- Service-manual figures differ slightly. Juno-60: 0.5 / 0.83 / 10 Hz. Juno-6: 0.4 / 0.67 /
  8.06 Hz. Use the **measured** values above as the defaults, and expose a small RATE trim in the
  popup to cover the spread.
- **I+II is not "I and II at once".** On the Juno-60 it's its own setting: a fast, shallow
  vibrato-like shimmer. Build it as a third mode, not by summing modes I and II.

### 1.2 Circuit facts that shape the sound

- **One 256-stage BBD per channel** (MN3009-class), effective clock about 70 kHz at the short
  end. Delay = 256 / (2·f_clk), so the clock runs from about 24 kHz at 5.35 ms to about 77 kHz
  at 1.66 ms.
- **12 dB/oct lowpass before the BBD** (anti-alias) plus a reconstruction lowpass after it.
  This darkens the wet signal and is a big part of why it sounds "smooth".
- **Stereo:** the right channel's delay modulation is **inverted** (180° from the left). Both
  outputs are dry + their own delayed copy. That's the famous wide-but-mono-safe image: the
  pitch wobble cancels when the two sides are summed.
- **No compander** in the Juno-60 chorus path, which is unusual for a BBD. It gets noisy (the
  hiss is part of the character) and the wet path saturates gently at high levels ("sawtooth
  comes out rounded").
- In modes I and II the delay range already reaches flanger territory at its short end. That
  fits well with a METAL knob that just keeps pushing the delay shorter.

### 1.3 Sources

- pendragon-andyh/Juno60 — Chorus README: https://github.com/pendragon-andyh/Juno60/blob/master/Chorus/README.md
- KVR DSP forum, "Juno 60 chorus": https://www.kvraudio.com/forum/viewtopic.php?t=489346
- Gearspace, "Detailed values of the Juno-106 chorus": https://gearspace.com/threads/detailed-values-of-the-juno-106-chorus.1367045/
- GroupDIY, "Roland Juno Chorus by TC": https://groupdiy.com/threads/roland-juno-chorus-by-tc.71088/page-2
- Roland Juno-106 owner's manual (chorus I/II behaviour): https://www.rolandcloud.com/getmedia/ca661bc1-7ef6-4479-9516-91263ee1647a/JUNO-106-Manual-E.pdf?ext=.pdf

> **Naming:** Heath is renaming effects to avoid other companies' product names before the beta.
> Keep "Juno"/"Roland" **out of all UI strings**. Use `CHO` / `CHORUS` in the UI and `I`, `II`,
> `I+II` for the modes. Citing it as the reference in code comments and this doc is fine.

---

## 2. Sound design: smooth ↔ metallic

**METAL = 0 is the Juno.** Nothing else changes at 0: MODE picks the measured rate and range,
there's no feedback, and the BBD filter sits at its dark setting.

**Turning METAL up** does four things together, all glided per-sample:

| As METAL goes 0 → 1 | Formula (starting point, tune by ear) | Why |
|---|---|---|
| Delay centre shrinks toward 0.5 ms | `centre = c0 · (0.5 / c0)^m` (exponential) | Short delay puts the comb teeth far apart (1/0.5 ms = 2 kHz), so it reads as pitch instead of doubling |
| Depth shrinks with it, a bit faster | `depth = d0 · (centre / c0) · (1 − 0.6·m)` | Keeps the ring on pitch instead of warbling |
| Feedback rises | `fb = 0.92 · m^1.2 · polarity` | This is where the metal comes from (resonant comb) |
| BBD filters open | `fc = 9 kHz · (16/9)^m` → 16 kHz | Lets the upper comb teeth ring bright |

Here `c0, d0` are the current MODE's centre and depth from §1.1, and `polarity` is +1 by
default, −1 with the popup's RING − toggle.

- **Polarity +** gives peaks at every multiple of 1/delay (about 2, 4, 6 kHz at 0.5 ms): a
  bright, bell/tube ring.
- **Polarity −** puts the peaks at odd multiples of 1/(2·delay) (about 1, 3, 5 kHz): hollow,
  more "robot" and square-ish.
- **MODE still matters at high METAL.** I and II give a slowly sweeping ring (jet/flange). I+II
  (9.75 Hz) at short delay with feedback gives a fast, ring-mod-like warble.

### 2.1 Prototype numbers

I checked the formulas above with a quick offline Python model (LFO frozen, sr 48 kHz, MIX 0.5,
output trim 0.75, wet compensation `sqrt(1 − |fb|)`, 80 Hz highpass in the loop):

| Setting | Comb peak | Peak freq | Peak-to-trough | Broadband RMS vs dry |
|---|---|---|---|---|
| METAL 0 (mode I) | +3.5 dB | ~570 Hz | deep notches (chorus) | −1.3 dB |
| METAL 0.5 | +4.7 dB | ~750 Hz | 15 dB | — |
| METAL 0.75 | +6.1 dB | ~1.2 kHz | 17 dB | — |
| METAL 1, RING + | +10.9 dB | ~1.9 kHz | 24 dB | −2.1 dB |
| METAL 1, RING − | +7.7 dB | high odd harmonics | 53 dB | — |

- The impulse ring at METAL 1 decays to −60 dB in about 27 ms, so **no idle tail machinery is
  needed** (see §5.4).
- A 4× impulse at METAL 1 in I+II stayed finite.
- The feedback curve is gentle through the middle (fb ≈ 0.40 at METAL 0.5). If the middle
  doesn't feel metallic enough, try `m^1.0`. It's a one-line change.

---

## 3. Controls

### 3.1 FX drawer cell (`ChorusPanel`, header-only like `RetrigPanel.h`)

Same packing as every other drawer cell: **pill (32) + gap + knob (53) + gap + knob (53) + gap +
"more" dot (20)** = 167 px.

| Element | Behaviour | Readout |
|---|---|---|
| Pill `CHO` | Enable toggle (`chorusEnabled`), same as the DLY pill | lit = on |
| Knob 1 **MODE** | Stepped: 0..1 mapped to 3 slots (I / II / I+II), same approach as `rtrgTime`→`noteIndex` | `I`, `II`, `I+II` |
| Knob 2 **METAL** | 0..1, double-click → 0 | `SMOOTH` at 0, else `METAL 37%` |
| "more" dot | Opens `ChorusEditorPanel` in a `PinnableCallout` | — |

### 3.2 Popup (`ChorusEditorPanel`, `lcd::HardwareLcdLookAndFeel`, title "CHORUS")

| Control | Range / default | Notes |
|---|---|---|
| MODE | I / II / I+II, default I | mirror of the drawer knob |
| METAL | 0..1, default 0 | mirror |
| MIX | 0..1, default **0.5** | 0.5 = the Juno's equal dry + wet. 1.0 = wet only (pure pitch vibrato) |
| RATE | 0..1, default 0.5 = measured rate | trim, ×0.5 .. ×2 (log) around the mode's rate. Readout in Hz |
| WIDTH | 0..1, default **1.0** | R-channel LFO phase offset = 0.5·WIDTH cycles (1.0 = inverted, Juno). 0 = mono-identical sides |
| HISS | 0..1, default **0** | BBD noise injected into the wet path only. Top ≈ −60 dBFS. 0 must stay bit-clean |
| RING | toggle `+` / `−`, default `+` | feedback polarity (§2) |

Defaults in bold keep the classic sound one click away: pill on, MODE I, METAL 0.

---

## 4. DSP architecture — `Source/ChorusEngine.h` (new)

### 4.1 Signal flow (per channel; L and R are identical except for LFO phase)

```
             ┌────────────────────────── dry ─────────────────────────────┐
             │                                                             ▼
in ──► (+) ──► 2-pole LP (fc) ──► soft-sat ──► [delay line] ──► Hermite read ──► wet ──► (×wetGain) ─► (+) ─► ×trim ─► out
        ▲         (BBD in)       (tanh-ish)                    @ centre+depth·LFO                     ▲
        │                                                              │                             (×dryGain)
        └── × fb ◄── 1-pole HP 80 Hz ◄── (optional post-LP) ◄──────────┘
                                          + HISS noise into the wet path
```

- The feedback tap comes **after** the read and goes back **before** the pre-LP. The LP, the
  saturator, and the 80 Hz HP all sit inside the loop. The HP is **required**: with positive
  feedback the comb also peaks at DC (gain 1/(1 − 0.92) = 12.5×), and the HP removes that. The
  LP sets how fast the high teeth decay, and the saturator keeps runaway bounded.
- Use a single lowpass (the pre-LP) inside the loop. An optional second one-pole on the wet
  output stands in for the reconstruction filter. Keep it outside the loop so METAL tuning stays
  predictable.

### 4.2 Parameter mapping (compute targets once per block, glide per sample)

```
modeIdx   = clamp(floor(mode01 * 3 * 0.9999), 0, 2)
rateHz    = {0.513, 0.863, 9.75}[modeIdx] * 2^((rate01 - 0.5) * 2)      // ×0.5..×2
c0, d0    = modeIdx < 2 ? (3.505, 1.845) : (3.5, 0.2)                   // ms
centreMs  = c0 * pow(0.5 / c0, m)
depthMs   = d0 * (centreMs / c0) * (1 - 0.6*m)
fb        = 0.92 * pow(m, 1.2) * (ringNegative ? -1 : 1)
fcHz      = 9000 * pow(16000.0/9000.0, m)          // clamp to 0.45·sr
dryGain   = min(1, 2*(1 - mix))
wetGain   = min(1, 2*mix) * sqrt(1 - |fb|)          // resonance compensation
trim      = 0.75
phaseOffR = 0.5 * width                              // cycles
```

- Smooth `centreMs`, `depthMs`, `fb`, `rateHz`, `dryGain`, `wetGain`, and `phaseOffR` with
  per-sample one-poles (τ ≈ 30 ms; about 50 ms for `centreMs` so a MODE switch *glides* instead
  of clicking). Recompute the biquad coefficients only when `fc` has moved more than ~0.5%
  (cache them the way `DirtStage` caches its drive curve).
- **Minimum delay guard:** `centre − depth` must stay ≥ 2 samples (the Hermite read needs one
  sample on either side). At 44.1 kHz, 0.5 ms is 22 samples, so there's plenty of room. Assert
  this in the test.

### 4.3 LFO

- One phase accumulator shared by both channels. R reads `phase + phaseOffR`.
- Triangle: `tri(p) = 4·|p − 0.5| − 1` (range −1..+1). A symmetric triangle inverted is the same
  as a half-cycle phase shift, so WIDTH 1.0 reproduces the Juno exactly.
- *Optional nuance (phase 2, only if A/B against a reference asks for it):* on the real unit the
  LFO drives the BBD **clock**, and delay ∝ 1/f_clk, so the delay sweep lingers slightly at the
  long end. You can model this by sweeping `f_clk` linearly and converting to delay. Skip it for
  v1.

### 4.4 Delay read

- Buffer size: the max delay is about 5.35 ms × 2 (RATE and glide headroom) = ~11 ms. Allocate
  **20 ms** in `prepare()`. At 192 kHz that's 3840 floats per channel, so it's tiny.
- **4-point Hermite (Catmull-Rom) interpolation.** Linear interpolation rolls off the highs by an
  amount that changes as the fractional delay moves, which is audible as a faint "swish" at 0.5
  Hz and makes the metallic peaks shimmer in level. Implement it in the chorus's own small delay
  struct. Don't modify `ReverbEngine.h::DelayLine`.

### 4.5 Soft saturation

`sat(x) = x` for |x| ≤ 0.5, then a smooth knee into ±1.2 (e.g. `0.5 + 0.7·tanh((|x| − 0.5)/0.7)`,
with the sign restored). Small signals stay linear and hot signals round off, which matches the
observed "rounded sawtooth". It also keeps the loop bounded at `fb = 0.92`.

### 4.6 HISS

- Use a cheap white-noise PRNG (xorshift32; no `juce::Random`, so it stays deterministic in
  tests). Tilt it toward pink with one one-pole, scale by `hiss01² · 0.001`, and add it at the
  BBD input.
- When `hiss01 == 0`, skip the PRNG entirely so the path stays bit-exact.

### 4.7 Engine API (mirror RetrigEngine / Mimeophon style)

```cpp
namespace r3wrk {
struct ChorusEngine
{
    static constexpr int    kMaxChannels = 2;
    static constexpr double kMaxDelayMs  = 20.0;
    static int         modeIndex(double mode01) noexcept;        // 0,1,2 — also used by the panel's readout
    static const char* modeName (int idx) noexcept;              // "I", "II", "I+II"
    static double      rateHz   (int idx, double rate01) noexcept;

    void prepare(double sampleRate, int numChannels);            // the only allocation
    void reset() noexcept;                                       // clears lines, filters, LFO phase = 0
    void snapToTargets() noexcept;                               // jump smoothers (offline tests / enable edge)

    struct Params { double mode01, metal01, mix01, rate01, width01, hiss01; bool ringNegative; };
    // In-place, numCh = 1 or 2. Mono: run the L path only (out = dry + L wet).
    void process(float* const* io, int numCh, int n, const Params&) noexcept;

    // Readback for tests / UI (optional)
    double currentCentreMs() const noexcept;
    double currentFeedback() const noexcept;
};
}
```

- Plain doubles for state. Per-sample math in double, stored to float, like the other engines.
- End of `process()`: if any filter or smoother state is non-finite, `reset()` (the same
  self-heal idea as `DirtStage`).
- Header comment: cite this doc (**`CHORUS_PLAN.md`**, kept at the repo root) and the sources in
  §1.3, matching how `MimeophonEngine.h` cites `MIMEOPHON_PLAN.md`.

---

## 5. Integration: exact touch points

### 5.1 `Source/AudioDocument.h` (next to the RTRG block, ~line 391)

```cpp
// CHORUS (r3wrk::ChorusEngine, ChorusEngine.h) -- BBD-style chorus, right after the filter
// (before RTRG). METAL 0 = the classic I / II / I+II chorus; up = shorter delay + feedback.
std::atomic<bool>   chorusEnabled { false };
std::atomic<double> chorusMode    { 0.0 };    // 0..1 -> I / II / I+II
std::atomic<double> chorusMetal   { 0.0 };
std::atomic<double> chorusMix     { 0.5 };
std::atomic<double> chorusRate    { 0.5 };    // trim, 0.5 = the mode's measured rate
std::atomic<double> chorusWidth   { 1.0 };
std::atomic<double> chorusHiss    { 0.0 };
std::atomic<bool>   chorusRingNeg { false };
```

### 5.2 `Source/PluginProcessor.h` / `.cpp`

- `#include "ChorusEngine.h"`. Members: `r3wrk::ChorusEngine chorusDsp; bool lastChorusEngaged = false;`
  plus `void applyChorus(juce::AudioBuffer<float>&, int numCh, int numSamples, bool freshPlayPass);`
- `prepareToPlay` (next to `rtrgDsp.prepare(...)`, ~line 166):
  `chorusDsp.prepare(sampleRate, juce::jmax(1, getTotalNumOutputChannels())); lastChorusEngaged = false;`
- **Call sites.** Insert `applyChorus` between the filter and RTRG in **both** branches. The
  scrub branch intentionally duplicates the chain (see CLAUDE.md), so keep that duplication:
  - Scrub branch (~line 615): `applyPlaybackFilter(...)` → **`applyChorus(buffer, numCh, numSamples, ! wasScrubbing);`** → `applyRetrig(...)`
  - Playing branch (~line 1111): after the chunked filter loop → **`applyChorus(buffer, numCh, numSamples, ! wasPlaying);`** → `applyRetrig(...)`
  - Update the chain comment in `processBlock`, and the chain line in `CLAUDE.md`:
    **NaN safety net → Dirt → filter → Chorus → RTRG → Mimeophon → Reverb/Plexiphon → Gain → NaN safety net → capture-output**.
- `applyChorus()` behaviour:
  - `engaged = chorusEnabled`. On the rising edge: `reset()` + `snapToTargets()` + a 10 ms
    wet fade-in. On the falling edge: keep processing while the wet/dry gains glide to bypass
    (~20 ms), then stop calling. After that the path is **bit-exact** (the buffer isn't touched).
    Easiest way: an internal `fadeGain` in the engine that ramps to 0, with `process()` reporting
    `busy()`, exactly like `DirtStage::busy()`.
  - `freshPlayPass`: **don't reset** (same lesson as Mimeophon/Reverb). Only the enable edge
    resets.
  - Unchunked, once per block. Not LFO-modulated (the LFO module is shelved).
- **Idle / stopped path: nothing to add.** The METAL 1 ring dies in under 30 ms, so, like the
  filter, the chorus just stops with playback. Don't add `chorusTailSamplesLeft`.
- **Monitor FX:** needs no work. Monitor-FX input is mixed in before Dirt, so it passes through
  the chorus automatically.
- **Save/Export bake:** out of scope. The drawer FX are live-only today ("phase 1: live
  monitoring, not baked"). Keep the chorus consistent with that and note it in the header
  comment.

### 5.3 State persistence (`PluginProcessor.cpp`)

- New magic: `kStateMagic = 0x52335755; // 'R3WU' - adds CHORUS`. Rename the old value to
  `kStateMagicR3WT = 0x52335754;` and add it to the accept list in `setStateInformation`.
- `getStateInformation`: append after `rtrgBpm` (and before the audio buffer):
  `chorusEnabled (bool), chorusMode, chorusMetal, chorusMix, chorusRate, chorusWidth, chorusHiss (doubles), chorusRingNeg (bool)`, tagged `// R3WU+`.
- `setStateInformation`:
  `const bool hasChorus = (magic == kStateMagic);` and
  `const bool hasRtrg = (hasChorus || magic == kStateMagicR3WT);` (the rest of the chain is
  unchanged). Read the block right after the `hasRtrg` block, with the defaults from §5.1 for
  older blobs, then `jlimit` and store. Add R3WU to the long "R3WS: adds…" history comment.
- `chorusEnabled` **is** persisted (like `mimeoEnabled`). It isn't a momentary latch like
  `rtrgLatched`.

### 5.4 UI: `Source/ChorusPanel.h` (new) + `Source/FxRow.h/.cpp`

- `ChorusPanel.h`: copy `RetrigPanel.h`'s structure (text helpers namespace `choText`,
  `ChorusEditorPanel`, `ChorusPanel` with its own `Pill` drawing `"CHO"` at `systemUIFont(10)`,
  `R3WRKLookAndFeel knobLnF`, `R3WRKIconOnlyLookAndFeel moreButtonLnf`, `PinnableCallout`, a
  15 Hz timer re-sync). MODE's knob readout uses `ChorusEngine::modeName(modeIndex(v))`.
  Header-only, so there's **no CMakeLists change** (headers aren't listed; only `.cpp` files are).
- `FxRow`: new member `ChorusPanel chorusPanel;` as the **1st slot** (left of RTRG), matching
  signal order. Update the class comment ("CHO | RTRG | Delay | RVB-or-PLX | Gain").
  - `resized()`: add the slot, and change the gap math from `3 * panelW` / `total / 3` to
    **`4 * panelW` / `total / 4`**, with the remainder still going to `lastGap`.
  - **Width check:** 4 × 167 = 668 px of panels. The editor's minimum width is 680
    (`setResizeLimits(680, …)`), and its default is 1000. Verify at 1000 px that Gain still
    lands under End. At 680 px, Gain is already dropped by the existing
    `full.getWidth() > lastGap + 40` guard. Make sure the last panel is clipped cleanly rather
    than overlapping. If it's too tight, the fallback is to have CHO and RTRG share a slot the
    way RVB/PLX do. **Ask Heath before doing that.**

### 5.5 Docs

- Add `CHORUS_PLAN.md` (this file) at the repo root.
- Update `CLAUDE.md`: the "What this is" FX list, the Playback chain line, and the FX drawer
  bullet list (add `Source/ChorusEngine.h` / `ChorusPanel.h`).

---

## 6. SmokeTest assertions (`Tests/SmokeTest.cpp`), written before wiring it live

Add `#include "../Source/ChorusEngine.h"` and a `-- CHORUS --` section next to the RTRG one.
Use sr = 48000 and also run the stress cases at 44100 and 96000.

1. **Mode table:** `modeIndex` maps 0 / 0.5 / 1.0 to 0 / 1 / 2; `modeName` gives "I", "II",
   "I+II"; `rateHz(idx, 0.5)` equals 0.513 / 0.863 / 9.75 Hz (±0.001).
2. **Juno delay range:** at METAL 0, mode I, log `currentCentreMs()` over 3 s and check that the
   min/max of the swept delay are 1.66 / 5.35 ms (±0.05). For I+II, 3.3 / 3.7 ms.
3. **LFO rate:** at MIX 1 (wet only), feed a 1 kHz sine in mode II and measure the
   instantaneous-frequency wobble period (or track the delay trace). It should be 1/0.863 s
   within ±2%.
4. **Stereo inversion:** WIDTH 1: the L and R delay traces are anti-correlated (corr < −0.95).
   WIDTH 0: L and R outputs are identical.
5. **Bypass is bit-exact:** disabled → output == input exactly. After a disable, once `busy()`
   goes false, the output is bit-exact again.
6. **HISS 0 is clean:** silence in → exactly 0.0 out, with the chorus on and METAL anywhere.
   HISS 1 → noise RMS between −75 and −55 dBFS.
7. **METAL mapping:** METAL 1 → centre 0.5 ms (±0.02), `|fb|` 0.92 (±0.01). RING − gives
   negative feedback.
8. **Metallic resonance exists:** METAL 1, RING +, frozen LFO (add a test-only `lfoFrozen` flag,
   or set RATE so low it doesn't move within the window). Take an impulse-response FFT, find a
   peak within ±5% of 1/centre ≈ 2 kHz, and check peak-to-trough ≥ 18 dB (the prototype got
   ~24 dB).
9. **Level sanity:** white noise at −14 dBFS RMS. With the chorus on at the defaults, output RMS
   is within ±3 dB of dry. At METAL 1, also within ±3 dB. A sine at the resonant peak gains no
   more than +12 dB.
10. **Stability stress:** impulse ×4 plus full-scale noise, sweeping METAL 0→1→0 and cycling
    MODE every 50 ms, RING flipping, sr ∈ {44.1k, 48k, 96k}, 10 s. The output stays finite with
    peak < 4.0, and 1 s of silence afterward decays below −90 dBFS.
11. **Min-delay guard:** at every setting, `centre − depth` ≥ 2 samples at 44.1 kHz.
12. **Mono:** numCh = 1 works, stays finite, and matches the L channel of a stereo run with the
    same input.

The build command for the tests is in `CLAUDE.md` (`R3WRKSmokeTest` target). All checks must
pass before touching `PluginProcessor`.

---

## 7. Build order & acceptance

| Step | Output | Done when |
|---|---|---|
| 1 | `CHORUS_PLAN.md` in repo | committed |
| 2 | `ChorusEngine.h` | compiles in `R3WRKSmokeTest` |
| 3 | SmokeTest §6 | **ALL CHECKS PASSED** |
| 4 | `AudioDocument` fields + `applyChorus` + both call sites + state `R3WU` | old projects (R3WT and earlier) load with the chorus off and defaults; a new project round-trips every chorus field |
| 5 | `ChorusPanel.h` + `FxRow` slot | pill/knobs/popup work; 1000 px layout still puts Gain under End; themes apply; no LookAndFeel leak asserts on close |
| 6 | Ear check (Heath) | METAL 0 mode I on a saw pad sounds like the classic chorus (wide, smooth, a little dark); METAL 1 rings clearly pitched; sweeping METAL or switching MODE never clicks |
| 7 | `CLAUDE.md` updated | chain + drawer lists mention CHORUS |

Build/test only on macOS (Xcode, JUCE 8.0.15) per `BUILD_ON_MACOS.md`. Remember the Intel-Mac
requirement for the beta. Nothing here is architecture-specific, but include the chorus in
whatever universal-binary check already exists.

---

## 8. Open questions / tuning knobs (for Heath, after first listen)

- **Feedback curve:** `m^1.2` vs `m^1.0` (more metal in the middle of the knob).
- **Dark filter value:** 9 kHz is an estimate for the Juno's pre-BBD LP. If METAL 0 sounds too
  bright or too dull next to a reference, this is the first constant to move (7–10 kHz range).
- **HISS default:** 0 (clean) is the plan. A real Juno always hisses a little. Consider ~0.15
  if the clean version feels too polite.
- **Slot:** CHO as its own 4th drawer slot (planned) vs sharing a slot with RTRG if the drawer
  gets too crowded.
- **Phase 2 ideas (not v1):** the BBD clock-domain LFO shaping (§4.3), true variable-rate BBD
  sampling/aliasing for a grittier "cheap chorus" mode, and a Juno-106-flavoured rate table as
  an alternate.
