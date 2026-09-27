#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>

// RTRG: an Octatrack-style buffer retrig, the first effect in the FX drawer (after the filter,
// before DLY). While latched on, the slice that played just before you clicked -- TIME long --
// repeats over and over, replacing the live signal; click again and the live signal comes back.
//
//   TIME  -- slice length. Synced: a note value against the tempo (the host's in VST/AU, the
//            RTRG popup's BPM in the Standalone). Free: 10 ms .. 1 s, log. Changing it while
//            stuttering re-cuts the SAME captured moment (the slice always ends where you clicked).
//   FADE  -- centre = plain, identical repeats. Anticlockwise: each repeat quieter (the OT's
//            retrig velocity curve going down). Clockwise: repeats start quiet and build up.
//
// Every repeat gets a short raised-cosine fade at both edges and latching on/off crossfades with
// the live signal, so the stutter never clicks. Header-only, plain floats, no allocation after
// prepare() -- the smoke test drives exactly this code.
namespace r3wrk
{
struct RetrigEngine
{
    static constexpr int kMaxChannels = 2;
    static constexpr double kMaxSliceSeconds = 1.0;

    // Synced TIME choices, longest to shortest (beats: 1 = a quarter note).
    struct NoteValue { const char* name; double beats; };
    static constexpr NoteValue kNotes[] = {
        { "1/2", 2.0 }, { "1/4", 1.0 }, { "1/4T", 2.0 / 3.0 }, { "1/8", 0.5 }, { "1/8T", 1.0 / 3.0 },
        { "1/16", 0.25 }, { "1/16T", 1.0 / 6.0 }, { "1/32", 0.125 }, { "1/32T", 1.0 / 12.0 }, { "1/64", 0.0625 },
    };
    static constexpr int kNumNotes = (int) (sizeof(kNotes) / sizeof(kNotes[0]));

    static int noteIndex(double time01) noexcept
    {
        return juce::jlimit(0, kNumNotes - 1, (int) std::floor(juce::jlimit(0.0, 1.0, time01) * kNumNotes * 0.9999));
    }
    static double freeMs(double time01) noexcept   // 10 ms .. 1000 ms, log
    {
        return 10.0 * std::pow(100.0, juce::jlimit(0.0, 1.0, time01));
    }
    // Slice length in seconds for the TIME knob.
    static double sliceSeconds(double time01, bool synced, double bpm) noexcept
    {
        const double s = synced ? kNotes[noteIndex(time01)].beats * 60.0 / juce::jlimit(20.0, 400.0, bpm)
                                : freeMs(time01) * 0.001;
        return juce::jlimit(0.005, kMaxSliceSeconds, s);
    }

    void prepare(double sampleRate, int numChannels)
    {
        sr = juce::jmax(8000.0, sampleRate);
        channels = juce::jlimit(1, kMaxChannels, numChannels);
        historyLen = (int) std::ceil(sr * kMaxSliceSeconds) + 16;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            history[c].assign((size_t) historyLen, 0.0f);
            capture[c].assign((size_t) historyLen, 0.0f);
        }
        edgeLen = juce::jmax(8, (int) std::lround(0.003 * sr));   // 3 ms repeat edges
        xfadeLen = juce::jmax(8, (int) std::lround(0.005 * sr));  // 5 ms latch crossfade
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
            std::fill(history[c].begin(), history[c].end(), 0.0f);
        writePos = 0; filled = 0;
        active = false; mixToRetrig = 0.0f;
        playPos = 0; repeat = 0;
    }

    // Processes a block in place. `latched` is the RTRG pill; the rest are the knobs.
    void process(float* const* io, int numCh, int n, bool latched, double time01, double fade01,
                 bool synced, double bpm) noexcept
    {
        numCh = juce::jmin(numCh, channels);
        const int sliceLen = juce::jlimit(8, historyLen - 16, (int) std::lround(sliceSeconds(time01, synced, bpm) * sr));
        const double amount = std::abs(juce::jlimit(0.0, 1.0, fade01) - 0.5) * 2.0;   // 0 = plain
        const double perRepeat = 1.0 - 0.6 * amount;                                    // gain factor per repeat
        const bool buildUp = fade01 > 0.5;
        const float step = 1.0f / (float) xfadeLen;

        for (int i = 0; i < n; ++i)
        {
            // Remember what's playing (the live signal), so a click can grab the last slice.
            for (int c = 0; c < numCh; ++c)
                history[c][(size_t) writePos] = io[c][i];
            const int nowPos = writePos;
            writePos = (writePos + 1) % historyLen;
            filled = juce::jmin(historyLen, filled + 1);

            if (latched && ! active && filled >= sliceLen)   // (latched before a full slice has
            {                                                 // played: wait, don't loop silence)
                // Freeze the last kMaxSliceSeconds so TIME can re-cut the same moment later.
                for (int c = 0; c < numCh; ++c)
                    for (int k = 0; k < historyLen; ++k)
                        capture[c][(size_t) k] = history[c][(size_t) ((nowPos + 1 + k) % historyLen)];
                active = true; playPos = 0; repeat = 0;
            }
            if (! latched && active && mixToRetrig <= 0.0f)
                active = false;

            mixToRetrig = juce::jlimit(0.0f, 1.0f, mixToRetrig + (latched ? step : -step));
            if (! active)
                continue;   // (mixToRetrig is already 0 here -- pure live signal)

            if (playPos >= sliceLen) { playPos = 0; ++repeat; }
            const int from = historyLen - sliceLen;   // the slice ends where you clicked
            double g = buildUp ? std::pow(perRepeat, (double) juce::jmax(0, 6 - repeat))
                               : std::pow(perRepeat, (double) repeat);
            const int edge = juce::jmin(edgeLen, sliceLen / 4);
            if (playPos < edge)                 g *= 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * playPos / edge);
            else if (playPos >= sliceLen - edge) g *= 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * (sliceLen - 1 - playPos) / edge);

            for (int c = 0; c < numCh; ++c)
            {
                const float wet = capture[c][(size_t) (from + playPos)] * (float) g;
                io[c][i] = io[c][i] * (1.0f - mixToRetrig) + wet * mixToRetrig;
            }
            ++playPos;
        }
    }

    bool isStuttering() const noexcept { return active; }

private:
    double sr = 44100.0;
    int channels = 1, historyLen = 1, edgeLen = 8, xfadeLen = 8;
    std::vector<float> history[kMaxChannels], capture[kMaxChannels];
    int writePos = 0, filled = 0, playPos = 0, repeat = 0;
    bool active = false;
    float mixToRetrig = 0.0f;
};
}
