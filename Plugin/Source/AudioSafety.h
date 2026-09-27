#pragma once
#include <JuceHeader.h>
#include <cmath>

// Small audio-thread safety helpers, header-only so the smoke test drives exactly this code.
namespace r3wrk
{
    // Replaces any NaN / infinity in the first `numCh` channels with silence; returns how many it
    // replaced. PluginProcessor runs this before Dirt (so no bad sample can get into Dirt's DC
    // blocker, the filter or the FX drawer effects -- all of which remember their past, so one bad
    // sample used to silence a channel until restart) and again just before the output.
    inline int zeroNonFinite(juce::AudioBuffer<float>& b, int numCh, int numSamples) noexcept
    {
        int fixed = 0;
        for (int ch = 0; ch < juce::jmin(numCh, b.getNumChannels()); ++ch)
        {
            float* x = b.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite(x[i])) { x[i] = 0.0f; ++fixed; }
        }
        return fixed;
    }

    // How many input channels really carry audio: 1 when there are two but the second is exact
    // digital silence while the first isn't -- a mono device (the MacBook's built-in mic) in the
    // Standalone, where JUCE hands R3WRK a silent right input. Overdub and Monitor then feed the
    // one live channel to both sides instead of recording/monitoring the left only.
    inline int liveInputChannels(const juce::AudioBuffer<float>& in, int numCh, int numSamples) noexcept
    {
        if (numCh < 2 || in.getNumChannels() < 2)
            return juce::jmin(numCh, in.getNumChannels());
        const float* r = in.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
            if (r[i] != 0.0f) return 2;
        const float* l = in.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
            if (l[i] != 0.0f) return 1;
        return 2;   // both silent: nothing to decide
    }
}
