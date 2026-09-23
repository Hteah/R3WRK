#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "ReverbEngine.h"   // reuse DelayLine, AllpassDiffuser, OnePoleLowpass, Biquad, shelf
                            // helpers, and chebyshevPerturb -- all three FDN-family engines in
                            // this project share these.

/**
    A stereo tape/BBD-style echo modelled on the Make Noise/Tom Erbe Mimeophon. Phase 1: the
    core delay engine (Zone, Rate, Repeats, Color, Halo, Mix). No Flip, Freeze, Skew, µRate, or
    Tempo sync yet -- each is a separate subsystem, deferred to a later pass (Tempo sync
    especially: this codebase has never wired up host-BPM tracking anywhere).

    See ~/Downloads/MIMEOPHON_PLAN.md for the full research notes this was built from. Sources:
      - Tom Erbe's own interview on the Mimeophon's design (Perfect Circuit, "SoundHack's Tom
        Erbe: Making the Mimeophon") -- the architectural description this engine follows,
        especially Halo sitting *inside* the feedback loop ("a reverb embedded in the delay"),
        not as a post-effect.
      - The Mimeophon manual's own Zone delay-time table -- see zoneRangeMs() below, its
        generating rule verified offline (Tests/SmokeTest.cpp) against all 8 published rows
        before being trusted anywhere else.
      - Elektromatic/mimeophon (GitHub), a fan Max/MSP patch -- useful only for cross-checking
        the Zone-time numbers (its own comments admit it puts Halo outside the feedback loop,
        contradicting Erbe's own description, so NOT followed for architecture).

    Deliberately not juce::dsp -- matches ReverbEngine.h/PlexiphonEngine.h's reasoning: plain
    doubles, no allocation once prepare() has sized the delay buffers, no locks, safe to tick
    from the audio thread, and it compiles into the headless smoke-test target.
*/
namespace r3wrk
{
    struct ZoneRange { double minMs, maxMs; };

    // Zone 0 is a fixed hardware-floor special case; zones 1-7 follow a clean rule derived from
    // the manual's own table (verified to reproduce all 8 rows within the table's own rounding
    // -- see the "Mimeophon Zone table" SmokeTest section): zone 1's min is a fixed base
    // (20.4ms); zones 2-7 double that base's min by 2^zone; every zone is 4x wide (min to max)
    // except zone 7, extended to 16x wide per the manual's own outlier max (41.796s).
    inline ZoneRange mimeoZoneRangeMs(int zoneIn) noexcept
    {
        const int zone = juce::jlimit(0, 7, zoneIn);
        if (zone == 0)
            return { 1.33, 20.4 };

        constexpr double base = 20.4;   // zone 1's min, ms
        const double minMs = base * std::pow(2.0, (double) (zone == 1 ? 0 : zone));
        const double widthRatio = (zone == 7) ? 16.0 : 4.0;
        return { minMs, minMs * widthRatio };
    }

    /**
        One channel's delay circuit: delay buffer, Halo diffusion (INSIDE the feedback loop, per
        Erbe's own description -- see this file's header comment), Color filtering, and Repeats
        (feedback gain + saturator). MimeophonEngine below owns two of these (L and R),
        independent of each other in phase 1 (no Skew yet -- that's exactly what a later pass
        adds: making L/R diverge).

        All the per-block-computed fields (set by MimeophonEngine::setParams(), not taken as
        processSample() arguments) match ErbeVerbReverb/PlexiphonEngine's own style.
    */
    struct MimeoChannel
    {
        static constexpr int kHaloStages = 3;
        // No ground-truth Halo diffuser timing exists for this module -- own deliberately
        // irregular spread, kept under ~15ms per stage, same convention as the other two
        // engines' diffusers.
        static constexpr double kHaloMs[kHaloStages] = { 3.7, 7.1, 11.3 };

        DelayLine       line;
        AllpassDiffuser halo[kHaloStages];
        OnePoleLowpass  repeatsSatHp;   // chebyshevPerturb's highpass state for Repeats' saturator
        OnePoleLowpass  colorSatHp;     // chebyshevPerturb's highpass state for Color's saturator
        Biquad          colorLow, colorHigh;

        double sampleRate = 44100.0;
        double delaySamplesSmoothed = 0.0;

        // Set once per setParams() call (block-rate), read every sample:
        double delaySamplesTarget = 0.0;
        double haloAmount = 0.0, haloGain = 0.0;
        double repeatsGain = 0.0, repeatsDriveAmt = 0.0;
        double colorSatAmount = 0.0;

        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1000.0, newSampleRate);
            const double maxMs = mimeoZoneRangeMs(7).maxMs + 10.0;   // headroom past zone 7's 41.8s
            line.prepare(sampleRate, maxMs);
            for (auto& stage : halo)
                stage.prepare(sampleRate, 20.0);
            reset();
        }

        void reset() noexcept
        {
            line.reset();
            for (auto& stage : halo) stage.reset();
            repeatsSatHp.reset();
            colorSatHp.reset();
            colorLow.reset();
            colorHigh.reset();
        }

        inline float processSample(float in) noexcept
        {
            // Glide the read position toward its target exponentially rather than snapping --
            // combined with DelayLine::readInterpolated(), this is what makes a Rate sweep or
            // Zone change sound like a smooth chorus/flange-style sweep instead of a zippered
            // jump. ~10ms time constant at 48kHz -- musical, not sluggish.
            constexpr double kGlideCoeff = 0.002;
            delaySamplesSmoothed += (delaySamplesTarget - delaySamplesSmoothed) * kGlideCoeff;

            const float delayed = line.readInterpolated(delaySamplesSmoothed);

            // Halo: diffusion cascade INSIDE the feedback loop (the one architectural detail
            // this build has to get right -- see this file's header comment), crossfaded in/out
            // by haloAmount rather than just varying the allpass gain, so Halo=0 reads as
            // cleanly transparent rather than faintly diffused.
            float diffused = delayed;
            for (int i = 0; i < kHaloStages; ++i)
            {
                const int delaySamplesHalo = (int) std::round(kHaloMs[i] * 0.001 * sampleRate);
                diffused = halo[i].process(diffused, delaySamplesHalo, haloGain);
            }
            const float postHalo = delayed + (float) haloAmount * (diffused - delayed);

            // Color: low-shelf + high-shelf pair plus a touch of saturation toward the dark end
            // -- own design, see this file's header comment.
            float colored = colorLow.processSample(postHalo);
            colored = colorHigh.processSample(colored);
            colored = chebyshevPerturb(colored, colorSatAmount, colorSatHp);

            // Repeats: feedback gain + the same saturator shape ErbeVerbReverb/PlexiphonEngine's
            // Decay uses, for stability up to and through self-oscillation.
            float fedBack = (float) (repeatsGain * colored);
            fedBack = chebyshevPerturb(fedBack, repeatsDriveAmt, repeatsSatHp);

            line.write(in + fedBack);

            // The wet tap: post-Halo, pre-feedback-gain -- "what you'd hear," not the raw
            // recirculating signal.
            return postHalo;
        }
    };

    /**
        The Mimeophon's stereo pair: two independent MimeoChannel circuits (no Skew yet -- see
        MimeoChannel's own comment), sharing the same Zone/Rate/Repeats/Color/Halo values.
        setParams() takes already-smoothed 0..1 knob positions, same division of labour as
        ErbeVerbReverb/PlexiphonEngine.
    */
    struct MimeophonEngine
    {
        MimeoChannel left, right;
        double sampleRate = 44100.0;
        double mixWet = 0.0, mixDry = 1.0;

        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1000.0, newSampleRate);
            left.prepare(sampleRate);
            right.prepare(sampleRate);
        }

        void reset() noexcept { left.reset(); right.reset(); }

        // zone01/rate01/repeats01/color01/halo01/mix01: 0..1 knob positions, already smoothed
        // by the caller.
        void setParams(double zone01, double rate01, double repeats01, double color01,
                       double halo01, double mix01) noexcept
        {
            zone01    = juce::jlimit(0.0, 1.0, zone01);
            rate01    = juce::jlimit(0.0, 1.0, rate01);
            repeats01 = juce::jlimit(0.0, 1.0, repeats01);
            color01   = juce::jlimit(0.0, 1.0, color01);
            halo01    = juce::jlimit(0.0, 1.0, halo01);
            mix01     = juce::jlimit(0.0, 1.0, mix01);

            // Zone: snaps to one of 8 positions (a rotary-knob-with-detents feel, matching the
            // real hardware's own physical Zone selector) -- see mimeoZoneRangeMs().
            const int zone = juce::jlimit(0, 7, (int) std::round(zone01 * 7.0));
            const auto range = mimeoZoneRangeMs(zone);

            // Rate: continuous within the Zone's range, log-mapped (delay-time knobs read more
            // naturally on a log scale, same reasoning as this project's other time/frequency
            // knobs) -- this build's own mapping choice, not specified by the manual.
            const double ms = range.minMs * std::pow(range.maxMs / range.minMs, rate01);
            const double delaySamplesTarget = ms * 0.001 * sampleRate;

            // Halo: crossfade amount + the diffuser cascade's own internal gain.
            const double haloAmount = halo01;
            const double haloGain   = juce::jlimit(0.0, 0.75, halo01 * 0.75);

            // Repeats: feedback gain + saturator drive, same shape as ErbeVerbReverb/
            // PlexiphonEngine's Decay -- pushed close to unity for genuine self-oscillation
            // ("Karplus-Strong... self-oscillation" per Erbe), the saturator catching the top of
            // the range for stability. A single delay tap's feedback loop is stable for any
            // |gain|<1 (no matrix/eigenvalue question the way Plexiphon's Plexus has), so this
            // cap can sit closer to 1.0 than Plexiphon's Decay could.
            const double repeatsGain = juce::jlimit(0.0, 0.98, repeats01 * 0.98);
            const double driveAmt    = juce::jlimit(0.0, 1.0, (repeatsGain - 0.85) * 7.0);

            // Color: dark (CCW) adds warmth/saturation and rolls off highs; bright (CW) opens up
            // and adds a touch of shelf lift -- own design, see this file's header comment.
            const double colorSigned = (color01 - 0.5) * 2.0;   // -1..+1 (-1 dark, +1 bright)
            const double lowGainDb   = juce::jlimit(-9.0, 3.0, -6.0 * colorSigned - 3.0);
            const double highGainDb  = juce::jlimit(-12.0, 6.0, 9.0 * colorSigned - 3.0);
            const double colorSatAmount = juce::jlimit(0.0, 0.4, juce::jmax(0.0, -colorSigned) * 0.4);

            for (auto* ch : { &left, &right })
            {
                ch->delaySamplesTarget = delaySamplesTarget;
                ch->haloAmount = haloAmount;
                ch->haloGain   = haloGain;
                ch->repeatsGain     = repeatsGain;
                ch->repeatsDriveAmt = driveAmt;
                ch->colorSatAmount  = colorSatAmount;
                setLowShelfCoeffs (ch->colorLow,  350.0,  0.7, lowGainDb,  sampleRate);
                setHighShelfCoeffs(ch->colorHigh, 4500.0, 0.7, highGainDb, sampleRate);
            }

            // Mix: equal-power (cosine) dry/wet crossfade, same as the other two engines.
            const double theta = mix01 * juce::MathConstants<double>::halfPi;
            mixDry = std::cos(theta);
            mixWet = std::sin(theta);
        }

        inline void processSample(float inL, float inR, float& outL, float& outR) noexcept
        {
            const float wetL = left.processSample(inL);
            const float wetR = right.processSample(inR);

            outL = (float) (mixDry * inL + mixWet * wetL);
            outR = (float) (mixDry * inR + mixWet * wetR);

            // Safety backstop, same spirit as the other two engines'.
            outL = juce::jlimit(-4.0f, 4.0f, outL);
            outR = juce::jlimit(-4.0f, 4.0f, outR);
        }
    };
}
