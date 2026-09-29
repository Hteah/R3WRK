# MNM_CHORUS_PLAN.md — clone the Monomachine FX-CHORUS into r3wrk

Research + implementation plan, written for Claude Code. **This replaces `CHORUS_PLAN.md`**
(the Juno/METAL design didn't sound right to Heath).

The approach is the one that already worked for this codebase's Monomachine filter
(`MonomachineFilterModel.h`, "fitted from white-noise-through-filter recordings" in the MNMFILTER
companion project): **measure the real thing, fit a model, generate a header, then build the
engine against golden test data.** There's no guessing a "chorus that sounds kind of like it".

The measurement rig ships alongside this plan as `mnmchorus-kit.zip`. See §5.

---

## 1. Why this can now be a real clone

Until this month you'd have needed the hardware. On **2026-09-28**, **Monomodule** was released:
a free, open-source (AGPL-3.0) plugin that runs the Monomachine's *own DSP program* instruction by
instruction inside an emulated Motorola DSP56300. Per its README, it "matches the hardware sample
for sample". It includes **Monomodule FX**, which has the FX-CHORUS machine as a plain audio
effect.

That gives us a sample-exact reference we can drive from a script: an impulse in, exact answers
out.

- Repo: https://github.com/shnolk/monomodule
- It needs Elektron's free **Monomachine OS 1.32B** `.syx` (from elektron.se). You download it
  once; Monomodule doesn't redistribute it.

### 1.1 Guardrails (read before touching anything)

- **Keep Monomodule out of r3wrk.** It's AGPL-3.0. Heath may close-source r3wrk later, so no
  Monomodule code, headers, or build links go in the r3wrk repo. The probe tool lives in a
  separate companion folder (`~/Documents/Claude/MNMCHORUS`, next to MNMFILTER). Only
  **measured numbers, fitted curves, and short golden test vectors** cross over.
- **Don't disassemble or port the Elektron DSP code.** It's a clean-room clone: black-box
  measurement only, same as the filter.
- **UI naming:** Heath is renaming effects before the beta. No "Monomachine"/"Elektron" in UI
  strings. Code comments and this doc may cite the reference.
- *(Not legal advice. This is the conservative line that keeps the close-source option open.)*

---

## 2. What's known about the MnM chorus (deep dive)

### 2.1 Official description (Monomachine manual OS1.32, Appendix A, FX machines)

- The manual calls it a **"2 x 3 tap stereo chorus"**. The Sound On Sound review repeats this as
  "a 2x3-tap stereo chorus". It most likely means 3 modulated taps per channel, but the manual
  doesn't define it. Measure it.
- The manual notes that the chorus "interacts with the original signal", and that for **MIX**
  "the best settings are often found in the middle". So the dry/wet *sum* (comb interaction) is
  part of the intended sound. MIX is a real crossfade, not just a send level.

| Param | Manual meaning | Default (Monomodule / OS) | Notes |
|---|---|---|---|
| **DEL** | delay time of the chorus taps | 64 | range in ms unknown → measure |
| **DEP** | modulation depth | 64 | |
| **SPD** | LFO speed | 64 | Hz law unknown → measure |
| **MIX** | dry ↔ wet | **127** (fully wet) | manual: sweet spots mid-range |
| **FB** | tap feedback | 0 | polarity/which tap → measure |
| **WID** | stereo width | 0 | 0 might be mono → measure |
| **LP** | low-pass **on the feedback signal** (not the output) | 127 (open) | |
| **INP** | input amplification | 64 (shown bipolar) | |

All parameters are raw 0..127, like the hardware.

### 2.2 Platform facts that shape the sound (from Monomodule's source and tests)

- **DSP56300, 24-bit fixed point, 44.1 kHz native.** Fixed point means hard saturation at ±1.0
  on internal values. The core runs with **arithmetic saturation mode (SR.SM) on**, and a test
  comment says "the chorus LFO relies on it". That's a strong hint the LFO is built from
  saturating arithmetic (for example a clipped or folded ramp → a triangle/trapezoid), not a
  sine table. The LFO-shape measurement will settle this.
- **Control rate is 16-frame blocks.** Parameters and any host-side modulation update once per
  16 samples (2756 Hz). After a kit load the host slews parameters in over about 0.2 s. If the
  chorus LFO is also block-rate, the delay moves in 16-sample steps, which is a subtle part of
  "digital Elektron" character. §5 measures this.
- **Path latency is 23 samples** (16 block + 7 kernel), and **THRU output is
  polarity-inverted**. The probe corrects both, so dry and wet line up.
- **INP gain law:** a Monomodule test comment says THRU gain = INP²·8, and that "127 would clip a
  0.25 input". So INP can push the machine into its own hard clip. That's part of the character
  at high FB, and `inp` measures it.
- **FX machines run inside a full track.** The output passes through the track's amp envelope,
  filter, EQ, and per-track delay. The probe sets these to neutral (envelope held open, no amp
  DIST, filter wide open, EQ flat, SRR 0, delay send 0), and `calibrate` verifies with the THRU
  machine that the track itself is transparent. Clone only the chorus machine.

### 2.3 What users say

The only substantive description found (Elektronauts): it "runs the gamut of chorus sounds from
smooth watery 80s tones and trippy pitch modulation". There are no published measurements
anywhere, which is why §5 exists.

### 2.4 Working hypothesis (to confirm or kill with data)

- 3 taps per channel, each on its own LFO phase (probably spread about 120°, "ensemble"-style,
  which explains "watery").
- The R channel is phase-offset from L, with the offset scaled by WID. At WID 0 that means mono.
- FB takes one tap (or the tap sum) back through a one-pole LP into the delay input.
- Linear-interpolated delay reads, a saturation-built triangle LFO, and hard clipping at the input
  and write stage.

Every item here has a measurement in §5 that answers it. The analysis code assumes none of it.

---

## 3. Unknowns → the measurement that answers each

| Unknown | Measurement (`measure.py …`) | Result it produces |
|---|---|---|
| Is the track neutral? latency / polarity right? | `calibrate` (THRU, impulse) | pass/fail + THRU gain |
| How many taps, their delays, gains, signs; DEL → ms law | `taps` (DEP 0, impulse, DEL 0..127) | per-tap ms + gain per channel |
| Interpolation type | `taps` (the `spread` field: 1 = integer, 2 = linear, >2 = cubic/allpass) | read type |
| MIX law (dry and wet gains) | `mix` | dry(MIX), wet(MIX) |
| WID: phase offset, cross-feed, or tap panning? | `width` (+ a left-only input) | tap amplitudes per side vs WID |
| LFO rate law, shape, depth law, per-tap and L/R phases | `lfo` (impulse train, track each tap) | Hz(SPD), shape, ms(DEP), phases |
| Block-rate stepping of the LFO/params | `lfo` residual + `glide` | step size/period, slew time |
| FB law, polarity, which delay is in the loop, LP law | `feedback` (impulse + noise loop-transfer G(f)) | gain(FB), sign, loop ms, fc(LP) |
| INP gain law + clip point | `inp` | gain(INP), THD vs level |
| Does the LFO restart on trigger? | add: two runs, same input, check the delay trace matches | phase-lock yes/no (needed for null tests) |

---

## 4. Plan of work

| Phase | Where | Output | Done when |
|---|---|---|---|
| **A. Rig** | MNMCHORUS (outside the repo) | probe built, `selftest.py` ALL PASS, `calibrate` passes on the real OS | THRU transparent |
| **B. Measure** | MNMCHORUS | `results/*.json` for every analysis, plus plots | every row in §3 answered |
| **C. Fit + export** | MNMCHORUS `fit.py` | `Source/MonomachineChorusModel.h` (GENERATED) + `Tests/MnmChorusGolden.h` | fit error within §7 limits |
| **D. Engine** | r3wrk | `Source/MnmChorusEngine.h` + SmokeTest | SmokeTest ALL PASS incl. golden comparisons |
| **E. Wiring + UI** | r3wrk | AudioDocument/state/processor/panel | §6 checklist |
| **F. Ear A/B** | Heath | same loop through Monomodule FX and r3wrk | Heath can't reliably tell them apart |

Heath reviews after B (the raw findings) before C and D get built. Findings can change the
architecture.

---

## 5. Phase A + B — the measurement kit (`mnmchorus-kit.zip`)

### 5.1 What's in it (already built and tested as far as possible)

- `probe/mnm_chorus_probe.cpp`: a CLI that takes raw float32 stereo in (44.1 kHz), runs it through
  Monomodule's `MonoVoice::processFx` with the FX-CHORUS (or THRU/FLANGER/PHASER) machine, and
  writes aligned, polarity-corrected output.
  - Options: `--syn DEL,DEP,SPD,MIX,FB,WID,LP,INP`, raw `--amp/--filt/--efx` pages, and
    `--change-at N --syn2 …` for mid-run parameter changes.
  - Neutral track defaults: AMP `0,127,127,127,0,64,64,0`; FILT `0,127,0,0,0,0,64,64`; EFX
    `64,64,0,64,0,0,0,127` (EQ flat, SRR 0, **delay send 0**).
  - **Verified:** compiles cleanly against Monomodule `main` headless (`-DMNM_BUILD_PLUGIN=OFF
    -DMNM_BUILD_TESTS=OFF`).
  - **Not yet verified:** a real run. The OS file couldn't be downloaded in the environment where
    this was written. The first job on Heath's Mac is `calibrate`.
- `measure.py`: all the analyses in §3. The key techniques:
  - **Tap finding:** impulse → peaks, with sub-sample position from the two-sample centroid (exact
    for linear interpolation).
  - **LFO tracking:** impulses every 20 ms, with each tap tracked across impulses (nearest
    neighbour). Rate comes from an FFT of the trace, shape from a least-squares sine vs triangle
    fit, plus depth and per-tap phase.
  - **Loop analysis without assuming a topology:** white noise, H0 at FB 0 vs H_fb.
    **G(f) = 1 − H0/H_fb** gives |G| = loop gain × |loop filter| (so FB law and LP cutoff), the
    phase slope gives the loop delay, and the sign gives polarity.
- `synth_ref.py` + `selftest.py`: a made-up 2×3-tap chorus with known answers.
  - **Verified: 23/23 checks pass.** It recovers tap delays exactly, LFO rate within 1%, triangle
    shape, depth within 0.03 ms, tap phases 0/0.33/0.67, loop gain 0.66 vs 0.68, loop LP 1061 vs
    1045 Hz, and loop delay 7.07 vs 7.05 ms.

### 5.2 Steps for Claude Code on Heath's Mac

1. Create `~/Documents/Claude/MNMCHORUS` from the kit. Clone Monomodule into it, add the probe as
   `src/probe`, and build `mnm-chorus-probe` (commands in the kit README). Get the OS `.syx` from
   Elektron (Heath downloads it).
2. `python3 selftest.py` → ALL PASS.
3. `measure.py … calibrate`. If it's not transparent, adjust the probe's AMP/FILT/EFX defaults.
   Likely suspects are DSND, amp DIST/VOL, and HOLD semantics. Re-run until the THRU impulse is a
   single peak at lag 0.
4. `measure.py … all`. Then add these, which the kit doesn't cover yet:
   - **DEL sweep at 1-step resolution** (0..127) and **DEP/SPD at 8-step resolution**, for curve
     fitting.
   - **Tap tracking at fast SPD:** shrink the impulse spacing to about 1.5× the longest tap so the
     LFO is sampled at 10+ points per cycle up to the top SPD.
   - **Block-rate check:** at slow SPD, look at `lfo` traces at full per-impulse resolution. If
     the delay moves in steps every 16 samples, record the step period and whether the tap read
     position jumps or glides.
   - **LFO retrigger:** does `noteOn` reset the LFO phase? (Run twice and compare.)
   - **Saturation map:** at FB 127, INP 127, check whether clipping happens at the input, at the
     delay write, or at the output. Compare where the waveform flattens with MIX 0 vs 127.
5. Write `FINDINGS.md` in MNMCHORUS: one table per §3 row, plots (delay traces, |G(f)|, gain
   laws), and a proposed structure diagram. **Stop and show Heath.**

---

## 6. Phase D + E — the r3wrk side

### 6.1 Codebase rules (from `CLAUDE.md`, non-negotiable)

- One plain `namespace r3wrk` struct, header-only, `noexcept`, plain doubles, no allocation after
  `prepare()`, **no base class/virtuals**, no `juce::dsp`. It must compile in the headless
  `R3WRKSmokeTest` target.
- **SmokeTest assertions before going live.** Every `setLookAndFeel(&x)` gets its matching
  `nullptr` in the destructor. State uses the magic-constant cascade, and existing reads are
  never reordered.
- Templates: `RetrigEngine.h`/`RetrigPanel.h` (newest effect, same chain spot, drawer
  pill + 2 knobs + dot, `PinnableCallout` popup), `applyMimeophon()` (reset only on the
  enable edge), `DirtStage.h` (per-sample ramps, bit-exact bypass, self-heal).

### 6.2 Generated model header (from `fit.py`)

`Source/MonomachineChorusModel.h`, shaped like `MonomachineFilterModel.h`:
`// GENERATED by MNMCHORUS/fit.py -- do not edit by hand`, with a header comment giving the run
counts and fit error. Functions take 0..1 knob values (value/127):

```cpp
namespace r3wrk { namespace mnmchorus {
constexpr int kTapsPerSide = 3;                         // (whatever §5 found)
double tapDelayMs (int tap, double del01) noexcept;     // static tap positions
double tapGain    (int side, int tap, double wid01) noexcept;
double lfoHz      (double spd01) noexcept;
double depthMs    (double dep01, double del01) noexcept;  // if depth scales with DEL, say so
double tapPhase   (int side, int tap, double wid01) noexcept;  // cycles
double dryGain    (double mix01) noexcept;  double wetGain (double mix01) noexcept;
double fbGain     (double fb01) noexcept;   int fbSign; int fbSourceTap;  // or "sum"
double loopLpHz   (double lp01) noexcept;
double inpGain    (double inp01) noexcept;
constexpr int  kControlBlock = 16;   constexpr bool kLfoBlockRate = /* measured */;
enum class LfoShape { Triangle, Sine, Trapezoid };  constexpr LfoShape kShape = /* measured */;
}}
```

Use tables with linear interpolation where a clean formula doesn't fit, same as `resonanceToQ`.

### 6.3 Engine: `Source/MnmChorusEngine.h`

- **Run at the host rate, with every time in ms/Hz**, not samples. This matches the other
  engines. At 48/96 kHz, linear interpolation at the higher rate is slightly brighter than the
  MnM's at 44.1 kHz. Add a matched one-pole on the wet path **only if** the A/B shows it's
  needed. Keep 44.1-internal resampling as a fallback, not the default.
- Per channel: an input gain + hard clip (if §5 shows input saturation), then the delay line
  (the chorus's own small struct; don't modify `ReverbEngine.h::DelayLine`). Read taps with
  whatever interpolation §5 found (linear is likely, so `readInterpolated` semantics). Then the
  tap sum × tap gains, the loop (fbGain·sign → one-pole LP → back into the write), clipping at
  the write if measured, and finally dry·dryGain + wet·wetGain.
- **LFO:** one phase accumulator; taps and R read at their measured phase offsets. Use the
  measured shape. If `kLfoBlockRate`, update LFO/delay targets every 16 samples at 44.1k
  equivalent (every `round(16·sr/44100)` samples) and hold or glide exactly as measured. That
  stepping is character, so keep it.
- **Param changes:** ramp per sample (≈20 ms), or use the measured host slew if `glide` shows one.
- **Enable edge:** reset + fade in over 10 ms. Disable: fade out over about 20 ms, then
  bit-exact bypass (`busy()` like `DirtStage`). With high FB the tail could last longer than the
  old design's. If `feedback` shows rings longer than ~100 ms at FB 127, mirror Mimeophon's idle
  tail handling (`*TailSamplesLeft`, silence detector) instead of cutting it off.
- End of `process()`: if any state is non-finite, `reset()`.

### 6.4 Integration touch points (checked against commit `a92680c`)

If the old CHORUS_PLAN build exists locally (`ChorusEngine.h`, `ChorusPanel.h`, `chorus*`
AudioDocument fields, state `R3WU`), **keep its wiring and replace its insides**. `R3WU` was never
pushed or shipped (origin/main is still `a92680c`), so it's fine to redefine R3WU's field list in
place. If a build with R3WU went to anyone, bump to `R3WV` instead.

- **`AudioDocument.h`** (next to the RTRG block, ~line 391): `chorusEnabled` (bool, false), and
  `chorusDel, chorusDep, chorusSpd, chorusMix, chorusFb, chorusWid, chorusLp, chorusInp` as
  `std::atomic<double>` 0..1, defaulting to the MnM's own defaults ÷127: DEL .504, DEP .504,
  SPD .504, **MIX .5** (hardware default is 1.0, fully wet, but the manual points to mid-range;
  **ask Heath** which he wants), FB 0, WID 0, LP 1.0, INP .504. Drop the Juno fields (mode/metal/
  rate/width/hiss/ringNeg).
- **`PluginProcessor.h/.cpp`:** `#include "MnmChorusEngine.h"`, a `chorusDsp` member,
  `prepare` next to `rtrgDsp.prepare` (~line 166), and `applyChorus()` **right after the filter,
  before RTRG** in both branches: scrub (~line 615) and playing (~line 1111). The duplication is
  intentional (see CLAUDE.md). Update the chain comment and CLAUDE.md: **Dirt → filter → Chorus →
  RTRG → Mimeophon → RVB/PLX → Gain**.
- **State:** `kStateMagic = 0x52335755 // 'R3WU' - adds CHORUS`, the old value becomes
  `kStateMagicR3WT`, `hasChorus = (magic == kStateMagic)`, `hasRtrg = (hasChorus || magic ==
  kStateMagicR3WT)`. Write the 9 fields after `rtrgBpm`, before the audio. Read them after the
  `hasRtrg` block with defaults for older blobs.
- **`Source/ChorusPanel.h`** (header-only, copy `RetrigPanel.h`): a pill `CHO`, and two drawer
  knobs, **DEL and FB**. That's the smooth ↔ metallic pair: long DEL with no FB is lush, short DEL
  with high FB rings. **Confirm with Heath.** SPD/DEP is the other obvious pair. Readouts are raw
  `0..127` like the hardware. The popup (`lcd::HardwareLcdLookAndFeel`, title "CHORUS") has all 8
  in two rows of 4, in hardware order: DEL DEP SPD MIX / FB WID LP INP.
- **`FxRow`:** new first slot (left of RTRG). The gap math changes from `3 * panelW` / `/3` to
  `4 * panelW` / `/4`. At the default 1000 px width, Gain must still land under End. At the
  minimum 680 px, check that it clips cleanly. If it's too tight, ask Heath about sharing a slot
  with RTRG.
- **No CMake change.** Headers aren't listed in `target_sources`.
- **Docs:** add this file to the repo root as `MNM_CHORUS_PLAN.md`, delete `CHORUS_PLAN.md`,
  add `FINDINGS.md`'s tables as an appendix, and update CLAUDE.md's FX lists.

---

## 7. Tests and acceptance

### 7.1 SmokeTest (`Tests/SmokeTest.cpp`, new `-- MNM CHORUS --` section)

1. **Model sanity:** the generated functions are monotonic where they're supposed to be, and
   finite at 0 and 1.
2. **Golden static taps:** for about 12 settings with DEP 0 (spread over DEL/MIX/WID/FB/LP),
   the engine's impulse response vs the golden vector in `Tests/MnmChorusGolden.h` (exported
   from probe runs at 44.1 kHz, first ~2048 samples) must reach **null residual ≤ −40 dB** at
   FB 0 and ≤ −30 dB with FB.
3. **Golden LFO:** for about 6 SPD/DEP settings, the engine's per-tap delay trace (from a test
   hook) vs the measured trace must match within **0.02 ms RMS** after phase alignment, with the
   rate within 0.5%.
4. **Loop:** the engine's |G(f)| (same noise method as measure.py) vs golden must be within 1 dB
   below 5 kHz.
5. **Bypass bit-exact,** and silence-in gives exactly 0 out.
6. **Stress:** FB 127, INP 127, full-scale noise + impulses, all params swept, sr ∈
   {44.1, 48, 96} kHz, 10 s. The output stays finite, peak < 4.0, and it decays after the input
   stops.
7. **Mono** (numCh 1): finite, and matches L of a stereo run.

### 7.2 Null test vs Monomodule (in MNMCHORUS, at 44.1 kHz)

Render the same 10 s drum loop and pad through `mnm-chorus-probe` and through r3wrk's engine
(via a tiny offline harness that includes only `MnmChorusEngine.h`, no Monomodule code needed in
r3wrk). If the LFO retriggers on noteOn, align LFO phase and require residual ≤ −30 dB. If it
doesn't, compare spectrograms and delay traces instead.

### 7.3 Ear A/B (Heath, Phase F)

Load Monomodule FX and r3wrk in the DAW on the same clip. Try:

- default settings;
- MIX 64;
- a long DEL at slow SPD ("watery 80s");
- fast SPD with high DEP ("trippy pitch");
- a short DEL with FB 110 and LP 127 (metallic ring);
- the same with LP 40 (dark ring).

---

## 8. Open questions for Heath

1. **MIX default:** the hardware's 127 (fully wet, like the MnM) or 64 (the manual's "sweet spot")?
2. **Drawer knobs:** DEL + FB (smooth ↔ metallic) or SPD + DEP (classic chorus)?
3. **Do you own a Monomachine?** If yes, a quick hardware cross-check of 3–4 settings (MIDI CC +
   audio interface) is worth doing to confirm Monomodule on the chorus specifically.
4. **Keep MnM quirks even if they sound "worse"?** That includes the block-rate LFO steps, hard
   clipping at high INP/FB, and the 44.1 kHz-flavoured interpolation. The default is keep, since
   that's the point of a clone.

---

## Sources

- Monomodule (source, README, `Machines.h`, `SpecData.cpp`, `tests/test_engine.cpp`): https://github.com/shnolk/monomodule · https://monomodule.com/
- Bedroom Producers Blog, Monomodule release (2026-09-28): https://bedroomproducersblog.com/2026/09/28/monomodule-elektron-monomachine/
- Sonicstate, Monomodule beta (2026-09-18): https://sonicstate.com/news/2026/09/18/elektron-monomachine-emulated
- Monomachine manual OS1.32, Appendix A FX machines (pp. 136–139): https://www.manualsdir.com/manuals/657174/elektron-monomachine.html?page=137 · official PDF: https://www.elektron.se/wp-content/uploads/2024/09/monomachine_manual_OS1.32.pdf
- Sound On Sound, Elektron Monomachine review: https://www.soundonsound.com/reviews/elektron-monomachine
- Elektronauts, "How's the reverb on the Monomachine (effects in general)?": https://www.elektronauts.com/t/hows-the-reverb-on-the-monomachine-effects-in-general/17679


---

# Appendix: FINDINGS (from the MNMCHORUS companion project)


Black-box measurements of the FX-CHORUS machine, run through Monomodule's DSP56300 emulation of the
real OS (`Elektron_SFX6-60_OS1.32B.syx`) with `mnm-chorus-probe`. All numbers are at 44.1 kHz, the
native rate. The raw JSON is in `results/`; the scripts are `measure.py` (Claude Desktop's kit, fixed
below), `lfo_sweep.py`, `loop_sweep.py` and `tap_lfo_fit.py`.

Parameters are raw 0..127, as on the hardware.

---

## 0. Rig problems found and fixed first (these would have poisoned every measurement)

| Problem | Symptom | Fix |
|---|---|---|
| **AMP DIST = 0** in the probe's "neutral" AMP page. DIST is bipolar (64 = none). | THRU output ≈ silent (gain 0.00004). | Probe AMP default → `0,0,127,127,64,64,64,0` (Monomodule's own FX-track AMP default). |
| **EFX DSND = 0**. DSND is bipolar (64 = no send). | A full-level track-delay echo at **249 ms**. | Probe EFX default → DSND 64. |
| **The track is never flat.** Even at neutral settings, THRU has a ~100 Hz low cut (−14 dB @ 50 Hz), about +2 dB mids, −7 dB @ 20 kHz, and a 2-sample pre-ring. | Tap shapes smeared; the sum-of-samples gain was wrong. | Every run is **deconvolved by THRU's impulse response** (Wiener-regularised; the IR keeps a 16-sample lead-in for the pre-ring). The track is exactly linear and time-invariant: superposition to −90 dB, same IR at any time and level to −82 dB. After deconvolution, noise through THRU matches the input to **−45.6 dB**. |
| **The chorus wet path is muted for about the first 0.1 s** after start-up. | The kit's impulses (at sample 64) gave **no wet at all** for DEL > 0, which looked like "the taps cancel". | Every run gets 0.5 s of silent pre-roll, stripped from the output. |
| The kit's LFO tracker assumed 3 separable taps and short runs; its feedback method assumed plain feedback. | Rates that jumped around, a loop delay of ~0 ms, a loop gain > 1. | Replaced by `lfo_sweep.py` (phase demodulation), `tap_lfo_fit.py` (three-tap model fit) and `loop_sweep.py` (an exact frequency-domain loop solve). |

`calibrate` now passes: THRU is transparent after deconvolution, and **CHORUS at MIX 0 is bit-identical
to THRU** (raw), with the dry impulse gain 0.996 after deconvolution.

---

## 1. Signal flow (proposed from the data)

```
            ┌───────────────────────────── dry ─────────────────────────────┐
in ─► ×2·(INP/64)² ─► hard clip ±1 ─► v ──┤                                       ×(1−MIX/127)─┐
                                         │                                                   ▼
                                         └─► (+) ─► hard clip ─► [delay line, ~2048 smp] ─┐   (+) ─► ×½ ─► out
                                   ×(1+k) ▲                                               │    ▲
                                          │     three taps, each ⅓, linear-interp reads    │    │
                                          │     at C + A·sin(2π f t + φ + 2πk/3) ──────────┴─► s ─► ×(MIX/127)
                                          │     C = 0.341 + 0.17575·DEL ms, A = (C−0.341)·DEP/128,
                                          │     f = 0.501·(SPD/64)² Hz, R: φ −= ¼·WID/127 cycle
                                          └── −k · onePoleLP(LP) ◄── s
                                                       k ≈ FB/128
```

The per-channel structure is the same for L and R. Only the LFO phase differs, by WID. There's no
L↔R cross-feed (crosstalk below −110 dB).

---

## 2. Taps and delay law (DEP 0: the three taps coincide)

| DEL | 0 | 8 | 16 | 32 | 64 | 96 | 120 | 127 |
|---|---|---|---|---|---|---|---|---|
| delay (ms) | 0.341 | 1.746 | 3.152 | 5.964 | 11.588 | 17.211 | 21.429 | 22.68 |

- **Exactly linear: `delay_ms = 0.341 + 0.17575 · DEL`** (15.04 samples + 7.75 samples per step).
- One combined tap per channel at DEP 0, gain **0.488**. That's about ½ × 0.976; the 0.976 is the
  linear-interpolation split at 511.03 samples.
- **Interpolation: linear.** The tap spreads over 2 samples in proportion to the fractional delay.
  There's no cubic ringing.
- Quirk: at **DEL 0** a small extra tap appears at **46.78 ms** (2063 samples, ≈ −23 dB). This looks
  like a read wrapping around a circular buffer of about 2048 samples. It's small; model it only if
  the A/B asks for it.

## 3. MIX

| MIX | 0 | 16 | 32 | 64 | 96 | 127 |
|---|---|---|---|---|---|---|
| dry | 0.996 | 0.871 | 0.747 | 0.498 | 0.249 | 0.000 |
| wet (tap sum) | 0 | 0.062 | 0.125 | 0.251 | 0.376 | 0.498 |

- **A linear crossfade: dry = 1 − MIX/127, wet = ½ · MIX/127.** Fully wet is **6 dB quieter** than dry.
  MIX 64 puts the dry at twice the wet level: that's the "interacts with the original signal" comb.

## 4. INP (input stage, before both dry and wet)

- **Gain = (INP/64)²** relative to INP 64, exactly: ×0.25 @ 32, ×1.56 @ 80, ×2.25 @ 96, ×3.94 @ 127.
  INP 0 is silence.
- **Hard clip** at the internal ±1.0 ceiling. Internal gain at INP 64 is 2× (Monomodule: "THRU
  gain = INP²·8"), so the input clips above ≈0.5 FS at INP 64, above ≈0.32 at INP 80, and
  above ≈0.13 at INP 127. THD with a 0.5 sine: 0 % @ 64, 15 % @ 80, 25 % @ 96, 35 % @ 127.
- The dry output is then ×½, so INP 64 = unity.

## 5. LFO

The phase-demodulation sweep (`lfo_sweep.py`) measures the *combined* delay of the three taps. For
three equal taps 120° apart, that combined signal moves at **3× the per-tap LFO rate**, so the
numbers below are the per-tap rate (the demodulated rate ÷ 3, confirmed independently by the §6
tap tracking: 0.0310 Hz at SPD 16).

| SPD | 0 | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 120 | 127 |
|---|---|---|---|---|---|---|---|---|---|---|
| per-tap Hz | stopped | 0.031 | 0.126 | 0.280 | 0.501 | 0.782 | 1.125 | 1.532 | 1.756 | ≈1.97 |

- **Rate = 0.501 · (SPD/64)² Hz** (a quadratic law, within about 1 % from 16 to 120). SPD 0 = LFO
  stopped.
- **Shape: sine.** A sine fits the per-sample trace, and the §6 cubic-sum fit leaves only 0.045 rms
  on a unit sine.
- **No block-rate stepping.** The per-sample delay trace (phase demodulation of a 3 kHz tone) moves
  smoothly every sample. There are no 16-sample stairs, so the "block-rate LFO" quirk from plan §2.2
  **does not apply** to this machine.
- **Phase-locked to the note trigger:** two runs are identical, so a sample-exact null test vs the
  emulator is possible.
- **WID = the R channel's LFO phase lag, linear, ¼ cycle (90°) at 127:** R = L − 0.25 · WID/127
  cycles. Depth and taps are unchanged. WID 0 = L and R identical. (Only defined modulo ⅓ cycle: with
  three identical taps, a ⅓-cycle shift just renames the taps, so that description is complete.)

## 6. Three taps, depth and phases (`tap_lfo_fit.py`)

Impulse tracking (50 ms spacing, SPD 16, 24 s) over DEL × DEP ∈ {16, 32, 64, 96, 127}². It's
analysed with assignment-free symmetric sums, because least-squares with tap assignment locked onto
wrong rates.

- **Three taps per channel, each gain ≈ 0.165 (⅓ of 0.5)**:
  `d_k(t) = C + A · sin(2π f t + φ + 2π k/3)`, k = 0, 1, 2 — **exactly 120° apart, same rate and
  depth.** Model check at every setting: Σ d_k is constant to ≤ 2.6 µs, and Σ (d_k − C)² to ≤ 1.2 %.
- **Depth: A = (C − 0.341 ms) · DEP/128 = 0.17575 · DEL · DEP/128 ms.** For example, DEL 64 / DEP 64
  gives ±5.61 ms. At DEP 127 each tap swings from ≈ 0.34 ms up to ≈ 2 × C. DEL 0 has no movement at
  any DEP.

| DEL \ DEP | 16 | 32 | 64 | 96 | 127 |
|---|---|---|---|---|---|
| 16 (C 3.152) | 0.348 | 0.697 | 1.394 | 2.091 | 2.766 |
| 64 (C 11.588) | 1.402 | 2.804 | 5.609 | 8.414 | 11.129 |
| 127 (C 22.676) | 2.787 | 5.575 | 11.148 | 16.723 | 22.124 |

(A in ms. The formula matches all 25 grid points within about 1 %.)

## 7. Feedback loop

Solved exactly from long impulse responses (DEP 0): `H = c·z⁻ᴰ / (1 − G(f)·z⁻ᴰ)`.

- **The tap sum feeds back** (second-generation echoes appear at every pairwise sum of the three tap
  delays, with the mixed pairs at 2× the same-tap pairs), **with negative polarity**.
- **Loop gain G(200 Hz) = −0.00777 · FB** (−0.50 @ 64, −0.93 @ 120; ≈ −FB/128).
- **First-pass gain rises with FB: c = 0.488 · (1 + 0.00775 · FB)** (0.73 @ 64, 0.94 @ 120). This is
  the same coefficient as the loop, which suggests `write = (1+k)·v − k·LP(s)`, k = FB/128. This
  first-pass boost does **not** go through LP (c is unchanged from LP 0 to 127).
- **LP = a one-pole low-pass in the feedback path only** (slope ≈ 5–6 dB/oct). The −3 dB point is
  roughly exponential:

| LP | 56 | 64 | 72 | 80 | 88 | 96 | 104 | 112 | 120–127 |
|---|---|---|---|---|---|---|---|---|---|
| fc (Hz) | ~420 | 589 | 942 | 1537 | 2404 | 3791 | 6290 | 10607 | open (≈ −1 dB @ 10 k) |

  That's about ×1.6 per 8 steps above LP 56. Below LP ~40, fc drops under ~150 Hz, too low to resolve
  with this method (it's clearly very dark; at LP 0 the repeats effectively vanish).
- **Saturation:** the loop hard-clips at the same ±1 ceiling. With sustained noise at FB 127, the
  output deviates from linear by −22 dB at 0.1 input and −9 dB at 0.2, and pins at the ceiling above
  that.
- **Tail length:** at FB 127 each repeat is ×0.99 → about 0.7 s to −60 dB at DEL 64, longer at
  larger DEL. R3WRK will need the Mimeophon-style idle tail (plan §6.3).

---

## 8. Against the working hypothesis (plan §2.4)

| Hypothesis | Result |
|---|---|
| 3 taps per channel, ~120° apart | ✅ exactly 120°, equal rate and depth; depth ∝ DEL · DEP |
| R phase-offset from L, scaled by WID | ✅ linear, ¼ cycle (90°) lag at WID 127 |
| FB = one tap or the sum → one-pole LP → back into the delay input | ✅ the **sum**, **negative** sign, plus a first-pass boost the plan didn't expect |
| Linear-interpolated reads | ✅ |
| Saturation-built triangle LFO | ❌ **sine** |
| Block-rate (16-sample) LFO stepping | ❌ smooth per sample |
| Hard clipping at the input and write | ✅ input clip measured; loop clips at the same ceiling |
| (new) wet muted for ~0.1 s after start-up | a startup artefact of the machine; not worth cloning |
