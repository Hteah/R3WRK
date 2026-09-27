#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// R3WRK's time/pitch engine -- deliberately an *effect*, not a transparent stretcher. Replaces
// Rubber Band (GPL), so every line here is R3WRK's own. Three stages, each bypassed when its
// knob is centred:
//
//   Speed   -- tape varispeed: the input is re-read at `speed` samples per sample (linear
//              interpolation, no anti-aliasing), so pitch and time move together like tape.
//   Stretch -- Paulstretch (Nasca Octavian Paul's algorithm, written from its description):
//              big sine-windowed frames, FFT, keep each bin's magnitude, randomise its phase,
//              inverse FFT, window again, overlap-add at half a window. The input advances
//              `hop / stretch` per frame, so time dilates while every frame's random phases
//              smear it into Paulstretch's washy, frozen texture. Always smears -- that's the point.
//   Pitch   -- a lo-fi granular shifter: two read taps into a short delay line, sliding at
//              (1 - ratio) and crossfaded with sin^2 windows half a grain apart (the classic
//              early-digital "doppler" shifter, warbly and metallic), then grit: sample-and-hold
//              at 14.7 kHz with no anti-aliasing, and 12-bit quantisation.
//
// Interface mirrors the four calls PluginProcessor already used with Rubber Band's real-time
// stretcher (getSamplesRequired / process / available / retrieve), so the drag-scan machinery
// in front of it (DragScanRender.h) is untouched. Header-only, plain doubles/floats, no
// allocation after prepare(), so the smoke test drives exactly this code offline.
namespace r3wrk
{
class LofiStretch
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate, int numChannels)
    {
        sr = juce::jmax(8000.0, sampleRate);
        channels = juce::jlimit(1, kMaxChannels, numChannels);

        // Window ~0.37 s at 44.1 kHz (a power of two >= 0.25 s): long enough for Paulstretch's
        // smear, short enough that a drag still responds.
        winSize = 1;
        while (winSize < (int) (0.25 * sr)) winSize <<= 1;
        hop = winSize / 2;

        inCap   = nextPow2(winSize * 8 + 65536);   // up to 4x speed reading a full window, + a process() chunk
        tapeCap = nextPow2(winSize * 4);
        outCap  = nextPow2(winSize * 2 + 65536);
        grainLen = (int) std::lround(0.046 * sr);  // ~46 ms grains
        delayCap = nextPow2(grainLen * 2 + 8);

        for (auto& c : ch)
        {
            c.in.assign((size_t) inCap, 0.0f);
            c.tape.assign((size_t) tapeCap, 0.0f);
            c.out.assign((size_t) outCap, 0.0f);
            c.accum.assign((size_t) winSize, 0.0f);
            c.frame.assign((size_t) winSize, {});
            c.delay.assign((size_t) delayCap, 0.0f);
        }
        window.resize((size_t) winSize);
        for (int n = 0; n < winSize; ++n)   // sine window: analysis x synthesis = sin^2, sums to 1 at a half-window hop
            window[(size_t) n] = (float) std::sin(juce::MathConstants<double>::pi * (n + 0.5) / winSize);
        prepareFft(winSize);
        reset();
    }

    // Clears all state (call setParams() first). With Stretch engaged the tape stream is primed
    // with half a window of silence so the first Paulstretch frame is centred on the first real
    // sample (latency() output samples later).
    void reset()
    {
        for (auto& c : ch)
        {
            std::fill(c.accum.begin(), c.accum.end(), 0.0f);
            std::fill(c.delay.begin(), c.delay.end(), 0.0f);
            c.heldValue = 0.0f;
        }
        inWritten = 0; readPos = 0.0;
        tapeWritten = 0; frameStart = 0.0;
        outRead = outWritten = 0;
        delayWrite = 0; grainPhase = 0.0; holdPhase = 1.0;
        finalSeen = false; flushTape = 0;
        rng = 0x2545F491u;
        paulActive = std::abs(stretch - 1.0) > 1.0e-3;
        pitchWet = std::abs(pitchSemis) > 1.0e-3 ? 1.0 : 0.0;
        if (paulActive)
            for (int n = 0; n < hop; ++n)
                pushTape(nullptr);   // half-window priming
    }

    // Tape speed (0.25-4), time stretch (0.25-50) and pitch (semitones). Callers ramp these.
    void setParams(double newSpeed, double newStretch, double newPitchSemis)
    {
        speed = juce::jlimit(0.05, 8.0, newSpeed);
        stretch = juce::jlimit(0.05, 100.0, newStretch);
        pitchSemis = newPitchSemis;
        pitchRatio = std::pow(2.0, pitchSemis / 12.0);
        // Paulstretch switches on (never off) mid-pass: dropping out of it would jump the time
        // base. At a stretch of exactly 1 it simply keeps smearing without dilating.
        if (! paulActive && std::abs(stretch - 1.0) > 1.0e-3)
            paulActive = true;
    }

    // Output samples between a sound going in and its centre coming out (Paulstretch's half
    // window; ~0 when Stretch is centred).
    int latency() const noexcept { return paulActive ? hop : 0; }

    // Input frames needed before more output can be made (0 = enough buffered already).
    int getSamplesRequired() const noexcept
    {
        const double inAvail = (double) inWritten - readPos;
        double tapeNeeded;
        if (paulActive)
            tapeNeeded = juce::jmax(0.0, (std::floor(frameStart) + winSize) - (double) tapeWritten);
        else
            tapeNeeded = outWritten - outRead > 0 ? 0.0 : 512.0;
        if (tapeNeeded <= 0.0)
            return 0;
        return juce::jmax(1, (int) std::ceil(tapeNeeded * speed - inAvail) + 2);
    }

    // Feeds `n` input frames (nullptr = silence). `final` = no more input will come: the tail
    // is flushed with silence so everything fed comes back out.
    void process(const float* const* input, int n, bool final)
    {
        for (int i = 0; i < n; ++i)
        {
            if (inWritten - (int64_t) readPos >= inCap - 2)
                break;   // input ring full (caller over-fed) -- drop rather than overwrite unread audio
            const int64_t w = inWritten & (inCap - 1);
            for (int c = 0; c < channels; ++c)
                ch[(size_t) c].in[(size_t) w] = input != nullptr ? input[juce::jmin(c, channels - 1)][i] : 0.0f;
            ++inWritten;
        }
        if (final && ! finalSeen)
        {
            finalSeen = true;
            flushTape = winSize + grainLen;   // enough silence to push the last frames and grains out
        }
        pump();
    }

    int available() const noexcept { return (int) (outWritten - outRead); }

    int retrieve(float* const* output, int n)
    {
        const int got = juce::jmin(n, available());
        for (int i = 0; i < got; ++i)
        {
            const int64_t r = (outRead + i) & (outCap - 1);
            for (int c = 0; c < channels; ++c)
                output[c][i] = ch[(size_t) c].out[(size_t) r];
        }
        outRead += got;
        pump();
        return got;
    }

private:
    struct Channel
    {
        std::vector<float> in, tape, out, accum, delay;
        std::vector<std::complex<float>> frame;
        float heldValue = 0.0f;
    };

    static int nextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

    // Moves audio input -> tape -> (Paulstretch) -> pitch -> output as far as buffers allow.
    void pump()
    {
        for (int guard = 0; guard < 1 << 20; ++guard)
        {
            bool progressed = false;

            // Speed: varispeed read of the input into the tape stream.
            // (Normally it waits for the next input frame to interpolate towards; once the input
            // is final, the last frame interpolates towards silence instead of being dropped.)
            while (tapeRoom() > 0 && (readPos + 1.0 < (double) inWritten
                                      || (finalSeen && readPos < (double) inWritten)))
            {
                const int64_t i0 = (int64_t) readPos;
                const float frac = (float) (readPos - (double) i0);
                const bool haveNext = i0 + 1 < inWritten;
                float s[kMaxChannels];
                for (int c = 0; c < channels; ++c)
                {
                    const auto& in = ch[(size_t) c].in;
                    const float a = in[(size_t) (i0 & (inCap - 1))];
                    const float b = haveNext ? in[(size_t) ((i0 + 1) & (inCap - 1))] : 0.0f;
                    s[c] = a + (b - a) * frac;
                }
                pushTape(s);
                readPos += speed;
                progressed = true;
            }
            // Past the last real input: pad the tape with silence to flush.
            while (finalSeen && flushTape > 0 && tapeRoom() > 0 && readPos >= (double) inWritten)
            {
                pushTape(nullptr);
                --flushTape;
                progressed = true;
            }

            if (paulActive)
            {
                while ((int64_t) std::floor(frameStart) + winSize <= tapeWritten && outRoom() >= hop)
                {
                    paulFrame();
                    progressed = true;
                }
            }
            else
            {
                while (tapeConsumed() < tapeWritten && outRoom() > 0)
                {
                    float s[kMaxChannels];
                    const int64_t t = tapeConsumed() & (tapeCap - 1);
                    for (int c = 0; c < channels; ++c)
                        s[c] = ch[(size_t) c].tape[(size_t) t];
                    frameStart += 1.0;
                    pitchAndPush(s);
                    progressed = true;
                }
            }
            if (! progressed)
                break;
        }
    }

    int64_t tapeConsumed() const noexcept { return (int64_t) std::floor(frameStart); }
    int tapeRoom() const noexcept { return tapeCap - (int) (tapeWritten - tapeConsumed()); }
    int outRoom() const noexcept { return outCap - (int) (outWritten - outRead); }

    void pushTape(const float* s)
    {
        const int64_t w = tapeWritten & (tapeCap - 1);
        for (int c = 0; c < channels; ++c)
            ch[(size_t) c].tape[(size_t) w] = s != nullptr ? s[c] : 0.0f;
        ++tapeWritten;
    }

    // One Paulstretch frame per channel (shared random phases keep the stereo image), then
    // `hop` finished samples on through the pitch stage.
    void paulFrame()
    {
        const int64_t start = (int64_t) std::floor(frameStart);
        for (int k = 0; k <= winSize / 2; ++k)
            phases[(size_t) k] = (float) (nextRandom() * juce::MathConstants<double>::twoPi);

        for (int c = 0; c < channels; ++c)
        {
            auto& cc = ch[(size_t) c];
            for (int n = 0; n < winSize; ++n)
                cc.frame[(size_t) n] = { cc.tape[(size_t) ((start + n) & (tapeCap - 1))] * window[(size_t) n], 0.0f };
            fft(cc.frame, false);
            for (int k = 0; k <= winSize / 2; ++k)
            {
                const float mag = std::abs(cc.frame[(size_t) k]);
                const float ph = (k == 0 || k == winSize / 2) ? 0.0f : phases[(size_t) k];
                cc.frame[(size_t) k] = std::polar(mag, ph);
                if (k > 0 && k < winSize / 2)
                    cc.frame[(size_t) (winSize - k)] = std::conj(cc.frame[(size_t) k]);   // keep it real
            }
            fft(cc.frame, true);
            const float norm = 1.0f / (float) winSize;
            for (int n = 0; n < winSize; ++n)
                cc.accum[(size_t) n] += cc.frame[(size_t) n].real() * norm * window[(size_t) n];
        }

        for (int n = 0; n < hop; ++n)
        {
            float s[kMaxChannels];
            for (int c = 0; c < channels; ++c)
                s[c] = ch[(size_t) c].accum[(size_t) n];
            pitchAndPush(s);
        }
        for (auto& c : ch)
        {
            std::copy(c.accum.begin() + hop, c.accum.end(), c.accum.begin());
            std::fill(c.accum.begin() + (winSize - hop), c.accum.end(), 0.0f);
        }
        frameStart += (double) hop / stretch;
    }

    // Granular pitch + grit, blended in/out over ~20 ms as the Pitch knob leaves/returns to 0.
    void pitchAndPush(const float* s)
    {
        const double ratio = pitchRatio;
        const double wetTarget = std::abs(pitchSemis) > 1.0e-3 ? 1.0 : 0.0;
        pitchWet += juce::jlimit(-1.0 / (0.02 * sr), 1.0 / (0.02 * sr), wetTarget - pitchWet);

        const int64_t w = delayWrite & (delayCap - 1);
        for (int c = 0; c < channels; ++c)
            ch[(size_t) c].delay[(size_t) w] = s[c];

        // Tap delays sweep 0..grainLen; the phase slides at (1 - ratio) per sample.
        grainPhase -= (ratio - 1.0) / (double) grainLen;
        grainPhase -= std::floor(grainPhase);
        const double ph[2] = { grainPhase, std::fmod(grainPhase + 0.5, 1.0) };

        holdPhase += 14700.0 / sr;
        const bool takeHold = holdPhase >= 1.0;
        if (takeHold) holdPhase -= std::floor(holdPhase);

        const int64_t o = outWritten & (outCap - 1);
        for (int c = 0; c < channels; ++c)
        {
            auto& cc = ch[(size_t) c];
            float wet = 0.0f;
            if (pitchWet > 0.0)
            {
                for (double p : ph)
                {
                    const double d = 1.0 + p * (double) grainLen;
                    const double rp = (double) delayWrite - d;
                    const int64_t i0 = (int64_t) std::floor(rp);
                    const float fr = (float) (rp - (double) i0);
                    const float a = cc.delay[(size_t) (i0 & (delayCap - 1))], b = cc.delay[(size_t) ((i0 + 1) & (delayCap - 1))];
                    const double win = std::sin(juce::MathConstants<double>::pi * p);
                    wet += (a + (b - a) * fr) * (float) (win * win);
                }
                if (takeHold)
                    cc.heldValue = std::round(wet * 2048.0f) / 2048.0f;   // 12-bit
                wet = cc.heldValue;
            }
            cc.out[(size_t) o] = (float) (s[c] * (1.0 - pitchWet) + wet * pitchWet);
        }
        ++delayWrite;
        ++outWritten;
    }

    double nextRandom() noexcept   // xorshift -- no locks or allocation on the audio thread
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (double) rng / 4294967296.0;
    }

    void prepareFft(int n)
    {
        twiddles.resize((size_t) n / 2);
        for (int k = 0; k < n / 2; ++k)
            twiddles[(size_t) k] = std::polar(1.0f, (float) (-juce::MathConstants<double>::twoPi * k / n));
        bitrev.resize((size_t) n);
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitrev[(size_t) i] = r;
        }
        phases.assign((size_t) n / 2 + 1, 0.0f);
    }

    // In-place iterative radix-2 FFT (inverse = conjugate twiddles, unscaled).
    void fft(std::vector<std::complex<float>>& a, bool inverse) const
    {
        const int n = (int) a.size();
        for (int i = 0; i < n; ++i)
            if (i < bitrev[(size_t) i]) std::swap(a[(size_t) i], a[(size_t) bitrev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < len / 2; ++j)
                {
                    auto w = twiddles[(size_t) (j * step)];
                    if (inverse) w = std::conj(w);
                    const auto u = a[(size_t) (i + j)], v = a[(size_t) (i + j + len / 2)] * w;
                    a[(size_t) (i + j)] = u + v;
                    a[(size_t) (i + j + len / 2)] = u - v;
                }
        }
    }

    double sr = 44100.0;
    int channels = 1, winSize = 16384, hop = 8192, inCap = 1, tapeCap = 1, outCap = 1;
    int grainLen = 2048, delayCap = 1;
    Channel ch[kMaxChannels];
    std::vector<float> window, phases;
    std::vector<std::complex<float>> twiddles;
    std::vector<int> bitrev;

    double speed = 1.0, stretch = 1.0, pitchSemis = 0.0, pitchRatio = 1.0;
    bool paulActive = false, finalSeen = false;
    int flushTape = 0;
    int64_t inWritten = 0, tapeWritten = 0, outRead = 0, outWritten = 0, delayWrite = 0;
    double readPos = 0.0, frameStart = 0.0, grainPhase = 0.0, holdPhase = 1.0, pitchWet = 0.0;
    uint32_t rng = 0x2545F491u;
};
}
