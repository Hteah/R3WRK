#include "PluginProcessor.h"
#include "DragScanRender.h"
#include "PluginEditor.h"
#include "OutputSettings.h"
#include <rubberband/RubberBandStretcher.h>
#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kStateMagic     = 0x5233574F;   // 'R3WO' - adds Mimeophon Ping-Pong
    constexpr int kStateMagicR3WN = 0x5233574E;   // 'R3WN' - adds Mimeophon Skew
    constexpr int kStateMagicR3WM = 0x5233574D;   // 'R3WM' - adds Mimeophon params
    constexpr int kStateMagicR3WL = 0x5233574C;   // 'R3WL' - adds Plexiphon params
    constexpr int kStateMagicR3WK = 0x5233574B;   // 'R3WK' - adds reverb Width
    constexpr int kStateMagicR3WJ = 0x5233574A;   // 'R3WJ' - adds reverb params
    constexpr int kStateMagicR3WI = 0x52335749;   // 'R3WI' - adds LFO modulation slots
    constexpr int kStateMagicR3WH = 0x52335748;   // 'R3WH' - adds loopReverse
    constexpr int kStateMagicR3WG = 0x52335747;   // 'R3WG' - adds the source file path
    constexpr int kStateMagicR3WF = 0x52335746;   // 'R3WF' - adds bakeLoopCrossfadeOnExport
    constexpr int kStateMagicR3WE = 0x52335745;   // 'R3WE' - adds loopCrossfadeMs
    constexpr int kStateMagicR3WD = 0x52335744;   // 'R3WD' - adds filterModel (MnM / Octatrack)
    constexpr int kStateMagicR3WC = 0x52335743;   // 'R3WC' - adds loopPingPong
    constexpr int kStateMagicR3WB = 0x52335742;   // 'R3WB' - MnM filter: Base/Width/HP Q/LP Q
    constexpr int kStateMagicR3WA = 0x52335741;   // 'R3WA' - OT filter + HP/LP slope (12/24dB)
    constexpr int kStateMagicR3W9 = 0x52335739;   // 'R3W9' - adds filterDrive
    constexpr int kStateMagicR3W8 = 0x52335738;   // 'R3W8' - filter is Base/Width, no drive
    constexpr int kStateMagicR3W7 = 0x52335737;   // 'R3W7' - mode/cutoff/res filter + playbackGainDb
    constexpr int kStateMagicR3W6 = 0x52335736;   // 'R3W6' - mode/cutoff/res filter, no gain
    constexpr int kStateMagicR3W5 = 0x52335735;   // 'R3W5' - no filter, no gain
    constexpr int kMaxStateChannels = 32;
}

R3WRKAudioProcessor::R3WRKAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    document.newEmptyDocument(2, 44100.0);

    // Seed this instance's Black Box duration from the persisted preference -- see
    // setBlackBoxDurationSecs()'s header comment on why that's not kept live across instances.
    blackBoxDurationSecs = juce::SharedResourcePointer<OutputSettings>()->blackBoxDurationSecs();
}

R3WRKAudioProcessor::~R3WRKAudioProcessor() = default;

bool R3WRKAudioProcessor::knobsEngaged(double speed, double pitch, double stretch)
{
    return std::abs(speed - 1.0) > 1.0e-4
        || std::abs(pitch) > 1.0e-4
        || std::abs(stretch - 1.0) > 1.0e-4;
}

void R3WRKAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    if (document.isEmpty())
        document.setSampleRate(sampleRate);

    reallocateBlackBoxBuffer();

    rtChannels = juce::jlimit(1, 2, juce::jmax(1, getMainBusNumOutputChannels()));

    using RB = RubberBand::RubberBandStretcher;
    rtStretcher = std::make_unique<RB>((size_t) sampleRate, (size_t) rtChannels,
                                       RB::OptionProcessRealTime | RB::OptionPitchHighConsistency);

    const int inScratch = juce::jmax(8192, juce::jmax(0, samplesPerBlock) * 8);
    rtScratchIn.setSize(rtChannels, inScratch);
    rtScratchOut.setSize(rtChannels, juce::jmax(8192, juce::jmax(0, samplesPerBlock) * 2));
    rtStretcher->setMaxProcessSize((size_t) inScratch);

    constexpr double ratioRampSeconds = 0.12;
    smoothedTimeRatio.reset(sampleRate, ratioRampSeconds);
    smoothedPitchScale.reset(sampleRate, ratioRampSeconds);
    smoothedTimeRatio.setCurrentAndTargetValue(1.0);
    smoothedPitchScale.setCurrentAndTargetValue(1.0);
    lastAppliedTimeRatio  = -1.0;
    lastAppliedPitchScale = -1.0;
    stretchRatioNeedsSnap = true;

    wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;
    stretcherPrimed = false;
    rtFinished = false;
    wasScrubbing = false;

    smoothedFilterBase.reset(sampleRate, 0.03);
    smoothedFilterWidth.reset(sampleRate, 0.03);
    smoothedFilterHpQ.reset(sampleRate, 0.03);
    smoothedFilterLpQ.reset(sampleRate, 0.03);
    smoothedFilterBase.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterBase.load()));
    smoothedFilterWidth.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterWidth.load()));
    smoothedFilterHpQ.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterHpQ.load()));
    smoothedFilterLpQ.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.filterLpQ.load()));
    playbackFilter[0].reset();
    playbackFilter[1].reset();
    lastFilterEngaged = false;
    modulatedFilter[0].reset();
    modulatedFilter[1].reset();
    modulatedFilterWasActive = false;

    smoothedGain.reset(sampleRate, 0.02);
    smoothedGain.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain((float) juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb,
                                                            document.playbackGainDb.load()),
                                       (float) AudioDocument::kMinGainDb));

    reverbDsp.prepare(sampleRate);
    constexpr double reverbRampSeconds = 0.05;
    smoothedReverbSize.reset(sampleRate, reverbRampSeconds);
    smoothedReverbAbsorb.reset(sampleRate, reverbRampSeconds);
    smoothedReverbDecay.reset(sampleRate, reverbRampSeconds);
    smoothedReverbTilt.reset(sampleRate, reverbRampSeconds);
    smoothedReverbMix.reset(sampleRate, reverbRampSeconds);
    smoothedReverbPredelay.reset(sampleRate, reverbRampSeconds);
    smoothedReverbWidth.reset(sampleRate, reverbRampSeconds);
    smoothedReverbSize.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbSize.load()));
    smoothedReverbAbsorb.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbAbsorb.load()));
    smoothedReverbDecay.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbDecay.load()));
    smoothedReverbTilt.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbTilt.load()));
    smoothedReverbMix.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbMix.load()));
    smoothedReverbPredelay.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbPredelay.load()));
    smoothedReverbWidth.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.reverbWidth.load()));
    reverbTailSamplesLeft = 0;
    reverbTailSilentSamples = 0;
    lastReverbEngaged = false;

    plexDsp.prepare(sampleRate);
    constexpr double plexRampSeconds = 0.05;
    smoothedPlexLevel.reset(sampleRate, plexRampSeconds);
    smoothedPlexPlexus.reset(sampleRate, plexRampSeconds);
    smoothedPlexSize.reset(sampleRate, plexRampSeconds);
    smoothedPlexDiffuse.reset(sampleRate, plexRampSeconds);
    smoothedPlexDecay.reset(sampleRate, plexRampSeconds);
    smoothedPlexColor.reset(sampleRate, plexRampSeconds);
    smoothedPlexMix.reset(sampleRate, plexRampSeconds);
    smoothedPlexLevel.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexLevel.load()));
    smoothedPlexPlexus.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexPlexus.load()));
    smoothedPlexSize.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexSize.load()));
    smoothedPlexDiffuse.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexDiffuse.load()));
    smoothedPlexDecay.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexDecay.load()));
    smoothedPlexColor.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexColor.load()));
    smoothedPlexMix.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.plexMix.load()));
    plexTailSamplesLeft = 0;
    plexTailSilentSamples = 0;
    lastPlexEngaged = false;

    mimeoDsp.prepare(sampleRate);
    constexpr double mimeoRampSeconds = 0.05;
    smoothedMimeoZone.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoRate.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoRepeats.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoColor.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoHalo.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoMix.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoSkew.reset(sampleRate, mimeoRampSeconds);
    smoothedMimeoZone.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoZone.load()));
    smoothedMimeoRate.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoRate.load()));
    smoothedMimeoRepeats.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoRepeats.load()));
    smoothedMimeoColor.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoColor.load()));
    smoothedMimeoHalo.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoHalo.load()));
    smoothedMimeoMix.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoMix.load()));
    smoothedMimeoSkew.setCurrentAndTargetValue(juce::jlimit(0.0, 1.0, document.mimeoSkew.load()));
    mimeoTailSamplesLeft = 0;
    mimeoTailSilentSamples = 0;
    lastMimeoEngaged = false;
}

void R3WRKAudioProcessor::releaseResources()
{
    rtStretcher.reset();
    wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;
    stretcherPrimed = false;
    rtFinished = false;
    wasScrubbing = false;
}

bool R3WRKAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    auto mono = juce::AudioChannelSet::mono();
    auto stereo = juce::AudioChannelSet::stereo();
    auto in = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return in == mono || in == stereo;
}

void R3WRKAudioProcessor::ensureRecordingCapacity(int numChannels, int64_t additionalSamples)
{
    int64_t needed = recordingWritePos + additionalSamples;
    if (recordingAccumulator.getNumChannels() != numChannels || (int64_t) recordingAccumulator.getNumSamples() < needed)
    {
        int64_t newCapacity = juce::jmax((int64_t) recordingAccumulator.getNumSamples(),
                                          (int64_t) (currentSampleRate * 4.0));
        while (newCapacity < needed)
            newCapacity *= 2;
        recordingAccumulator.setSize(numChannels, (int) newCapacity, true, true, true);
    }
}

//==============================================================================
// Loop-crossfade envelope: raised-cosine (equal-power) gain for region-relative frame `rp`
// -- ramps 0->1 over the first `fadeLen` frames of the region and 1->0 over the last
// `fadeLen`, else 1. `fadeLen` is pre-clamped to regionLen/2 by the caller.
static inline double loopFadeGain(int64_t rp, int64_t regionLen, int fadeLen) noexcept
{
    if (fadeLen <= 0) return 1.0;
    double x;
    if (rp < fadeLen)                        x = (double) rp / (double) fadeLen;
    else if (rp >= regionLen - fadeLen)      x = (double) (regionLen - 1 - rp) / (double) fadeLen;
    else                                     return 1.0;
    x = juce::jlimit(0.0, 1.0, x);
    const double s = std::sin(0.5 * juce::MathConstants<double>::pi * x);
    return s * s;
}

// Walks [regionStart, regionEnd) from `pos` in direction `dir` (+1 forward, -1 backward),
// copying up to `count` frames into dst[dstOffset..]. `loop` wraps head-to-tail; `pingPong`
// bounces at both ends instead -- `dir` flips, and neither endpoint is repeated, so the cycle
// is 0,1,..,L-1,L-2,..,1 with period 2L-2 (the same shape as the Sieve editor's ping-pong).
// `dir` only ever goes -1 when `pingPong` is true. `fadeLen` > 0 applies the loop-crossfade
// envelope to the copied audio by its region position. Returns frames written; a short
// return means a non-looping region ran out.
static int gatherRegion(juce::AudioBuffer<float>& dst, int dstOffset, int count, int dstChannels,
                        const juce::AudioBuffer<float>& docBuf,
                        int64_t& pos, int& dir,
                        int64_t regionStart, int64_t regionEnd, bool loop, bool pingPong,
                        bool reverseLoop, int fadeLen)
{
    const int srcChans = docBuf.getNumChannels();
    if (srcChans <= 0 || regionEnd <= regionStart)
        return 0;
    const int64_t regionLen = regionEnd - regionStart;

    auto applyFade = [&](int atFrame, int chunk, int64_t rp0, int step)
    {
        if (fadeLen <= 0) return;
        float* wp[8];
        const int nw = juce::jmin(dstChannels, 8);
        for (int ch = 0; ch < nw; ++ch) wp[ch] = dst.getWritePointer(ch, dstOffset + atFrame);
        for (int j = 0; j < chunk; ++j)
        {
            const double g = loopFadeGain(rp0 + (int64_t) step * j, regionLen, fadeLen);
            if (g < 1.0)
                for (int ch = 0; ch < nw; ++ch)
                    wp[ch][j] *= (float) g;
        }
    };

    int written = 0;
    while (written < count)
    {
        if (dir > 0)
        {
            if (pos >= regionEnd)
            {
                if (pingPong)    { dir = -1; pos = regionEnd - 2; continue; }   // reflect at the top
                // Reverse-loop mode switched on WHILE playback is already going forward (e.g.
                // pressing Play with nothing selected, then cycling the Loop button mid-playback
                // -- direction otherwise only ever starts backward at a fresh Play press, see
                // processBlock's startReversed) -- flip to backward here too, mirroring the
                // backward branch's own reverseLoop wrap below, instead of silently treating
                // reverseLoop exactly like a plain forward loop until Play is pressed again.
                if (reverseLoop) { dir = -1; pos = regionEnd;     continue; }
                if (loop)        { pos = regionStart; continue; }
                break;
            }
            const int chunk = (int) juce::jmin((int64_t) (count - written), regionEnd - pos);
            for (int ch = 0; ch < dstChannels; ++ch)
                dst.copyFrom(ch, dstOffset + written, docBuf,
                             juce::jmin(ch, srcChans - 1), (int) pos, chunk);
            applyFade(written, chunk, pos - regionStart, +1);
            pos     += chunk;
            written += chunk;
        }
        else   // backward: either the ping-pong return leg (stopping one frame short of
               // regionStart before reflecting forward) or a reverse loop, which instead
               // wraps tail-to-head and keeps playing backward -- the mirror image of a
               // plain forward loop's wrap to regionStart
        {
            if (pos <= regionStart)
            {
                if (reverseLoop)
                {
                    pos = regionEnd;           // wrap back to the tail, still playing backward
                    continue;
                }
                // Ping-pong's return leg bounces back to forward here -- pingPong implies loop
                // (see its own computation at the call site), so this is unreachable with loop
                // off. Mirrors the forward branch's own `if (loop) ... else break` shape: with
                // loop truly off (backward only via a reverse-loop pass that was then switched
                // off mid-playback), there's no mode left to bounce back into -- stop here
                // instead of unconditionally flipping to forward and continuing forever, which
                // is what silently made "loop off" never actually stop once direction had ever
                // gone backward.
                if (loop)
                {
                    dir = 1;
                    pos = regionStart;             // the next frame forward plays
                    continue;
                }
                break;
            }
            const int chunk = (int) juce::jmin((int64_t) (count - written), pos - regionStart);
            const int64_t from = pos - chunk;  // copy [from+1 .. pos] forward, then reverse it
            for (int ch = 0; ch < dstChannels; ++ch)
            {
                dst.copyFrom(ch, dstOffset + written, docBuf,
                             juce::jmin(ch, srcChans - 1), (int) (from + 1), chunk);
                float* w = dst.getWritePointer(ch, dstOffset + written);
                std::reverse(w, w + chunk);
            }
            applyFade(written, chunk, pos - regionStart, -1);   // dst frame 0 == region pos `pos`
            pos     -= chunk;
            written += chunk;
        }
    }
    return written;
}

void R3WRKAudioProcessor::renderPlaybackDirect(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                               const juce::AudioBuffer<float>& docBuf,
                                               int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                                               bool loop, bool pingPong, bool reverseLoop, int loopFadeLen)
{
    if (docBuf.getNumChannels() <= 0)
        return;

    const int written = gatherRegion(out, 0, numSamples, numCh, docBuf,
                                     pos, dir, regionStart, regionEnd, loop, pingPong, reverseLoop, loopFadeLen);

    document.playhead.store(pos, std::memory_order_relaxed);
    if (! loop && written < numSamples)
        document.isPlaying.store(false, std::memory_order_relaxed);
}

// Real-time tape / pitch / stretch path. All three knobs are one RubberBand pass:
//   timeRatio  = stretch / speed   (speed compresses time like tape, stretch dilates it)
//   pitchScale = speed * 2^(pitch/12)   (pitch tracks the tape speed, then the extra shift;
//                                        stretch does NOT touch pitch)
void R3WRKAudioProcessor::renderPlaybackStretched(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                                  const juce::AudioBuffer<float>& docBuf,
                                                  int64_t& pos, int& dir, int64_t regionStart, int64_t regionEnd,
                                                  bool loop, bool pingPong, bool reverseLoop, int loopFadeLen,
                                                  double speed, double pitch, double stretch)
{
    if (rtStretcher == nullptr || docBuf.getNumChannels() <= 0 || regionEnd <= regionStart)
        return;

    speed   = juce::jlimit(AudioDocument::kMinSpeed,   AudioDocument::kMaxSpeed,   speed);
    pitch   = juce::jlimit(AudioDocument::kMinPitch,   AudioDocument::kMaxPitch,   pitch);
    stretch = juce::jlimit(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, stretch);

    updateStretchRatios(numSamples, speed, pitch, stretch);

    const int rc = rtChannels;
    const int inCap  = rtScratchIn.getNumSamples();
    const int outCap = rtScratchOut.getNumSamples();

    int produced = 0;
    int guard = numSamples * 4 + 64;   // hard cap on iterations, just in case

    while (produced < numSamples && --guard > 0)
    {
        const int avail = (int) rtStretcher->available();
        if (avail > 0)
        {
            const int n = juce::jmin(avail, numSamples - produced, outCap);
            float* op[2] = { rtScratchOut.getWritePointer(0),
                             rtScratchOut.getWritePointer(rc > 1 ? 1 : 0) };
            rtStretcher->retrieve(op, (size_t) n);
            for (int ch = 0; ch < numCh; ++ch)
                out.copyFrom(ch, produced, rtScratchOut, juce::jmin(ch, rc - 1), 0, n);
            produced += n;
            continue;
        }

        if (rtFinished)   // final block already sent and the stream has drained
        {
            document.isPlaying.store(false, std::memory_order_relaxed);
            break;
        }

        int req = (int) rtStretcher->getSamplesRequired();
        req = juce::jlimit(1, inCap, req > 0 ? req : 256);

        const int gathered = gatherRegion(rtScratchIn, 0, req, rc, docBuf,
                                          pos, dir, regionStart, regionEnd, loop, pingPong, reverseLoop, loopFadeLen);
        const bool regionEnded = (gathered < req);
        for (int ch = 0; ch < rc; ++ch)
            if (gathered < req)
                rtScratchIn.clear(ch, gathered, req - gathered);

        // Just released a drag on a stretched file -- blend what the drag renderer would have
        // fed next out of the input (see releaseInputTail's comment).
        if (releaseInputTailRemaining > 0)
        {
            const int n = juce::jmin(releaseInputTailRemaining, gathered);
            const int done = releaseInputTailLen - releaseInputTailRemaining;
            for (int i = 0; i < n; ++i)
            {
                const double x = (double) (done + i) / (double) releaseInputTailLen;
                const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * x);
                const float g = (float) (sn * sn);
                for (int ch = 0; ch < rc; ++ch)
                    rtScratchIn.setSample(ch, i, rtScratchIn.getSample(ch, i) * g
                                                 + releaseInputTail.getSample(juce::jmin(ch, releaseInputTail.getNumChannels() - 1), done + i) * (1.0f - g));
            }
            releaseInputTailRemaining -= n;
        }

        const float* ip[2] = { rtScratchIn.getReadPointer(0),
                               rtScratchIn.getReadPointer(rc > 1 ? 1 : 0) };
        const bool finalNow = regionEnded && ! loop;
        rtStretcher->process(ip, (size_t) req, finalNow);
        if (finalNow)
            rtFinished = true;
    }

    document.playhead.store(pos, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::updateStretchRatios(int numSamples, double speed, double pitch, double stretch)
{
    speed   = juce::jlimit(AudioDocument::kMinSpeed,   AudioDocument::kMaxSpeed,   speed);
    pitch   = juce::jlimit(AudioDocument::kMinPitch,   AudioDocument::kMaxPitch,   pitch);
    stretch = juce::jlimit(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, stretch);

    // Ramp the ratios instead of stepping them, so a knob drag mid-playback doesn't machine-gun
    // RubberBand with abrupt changes (that's the pops/crackle). Sampled once per block; snapped
    // straight to target on a fresh play pass so playback starts at the right ratio.
    const double targetTimeRatio  = stretch / juce::jmax(1.0e-4, speed);
    const double targetPitchScale = speed * std::pow(2.0, pitch / 12.0);
    if (stretchRatioNeedsSnap)
    {
        smoothedTimeRatio.setCurrentAndTargetValue(targetTimeRatio);
        smoothedPitchScale.setCurrentAndTargetValue(targetPitchScale);
        stretchRatioNeedsSnap = false;
    }
    else
    {
        smoothedTimeRatio.setTargetValue(targetTimeRatio);
        smoothedPitchScale.setTargetValue(targetPitchScale);
    }
    const double tr = smoothedTimeRatio.skip(numSamples);
    const double ps = smoothedPitchScale.skip(numSamples);
    if (std::abs(tr - lastAppliedTimeRatio) > 1.0e-4 * juce::jmax(1.0, tr))
    {
        rtStretcher->setTimeRatio(tr);
        lastAppliedTimeRatio = tr;
    }
    if (std::abs(ps - lastAppliedPitchScale) > 1.0e-4 * juce::jmax(1.0, ps))
    {
        rtStretcher->setPitchScale(ps);
        lastAppliedPitchScale = ps;
    }
}

void R3WRKAudioProcessor::renderDragScanStretched(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                                  const juce::AudioBuffer<float>& docBuf, double& pos,
                                                  double startA, double endA, double startB, double endB,
                                                  bool loop, double maxFadeLen,
                                                  double speed, double pitch, double stretch)
{
    if (rtStretcher == nullptr || docBuf.getNumChannels() <= 0)
        return;

    updateStretchRatios(numSamples, speed, pitch, stretch);

    const int rc = rtChannels;
    const int inCap  = rtScratchIn.getNumSamples();
    const int outCap = rtScratchOut.getNumSamples();

    // The region moves across the block in *output* time, but input is pulled in chunks of
    // whatever RubberBand asks for -- so place each chunk's edges by how much of this block's
    // expected input (numSamples / time ratio) has been fed so far.
    const double expectedIn = (double) numSamples / juce::jmax(1.0e-4, lastAppliedTimeRatio);
    double fed = 0.0;
    auto edgeAt = [&](double f, double a, double b) { return a + (b - a) * juce::jlimit(0.0, 1.0, f); };

    int produced = 0;
    int guard = numSamples * 4 + 64;
    while (produced < numSamples && --guard > 0)
    {
        const int avail = (int) rtStretcher->available();
        if (avail > 0)
        {
            const int n = juce::jmin(avail, numSamples - produced, outCap);
            float* op[2] = { rtScratchOut.getWritePointer(0),
                             rtScratchOut.getWritePointer(rc > 1 ? 1 : 0) };
            rtStretcher->retrieve(op, (size_t) n);
            for (int ch = 0; ch < numCh; ++ch)
                out.copyFrom(ch, produced, rtScratchOut, juce::jmin(ch, rc - 1), 0, n);
            produced += n;
            continue;
        }
        if (rtFinished)
        {
            document.isPlaying.store(false, std::memory_order_relaxed);
            break;
        }

        int req = (int) rtStretcher->getSamplesRequired();
        req = juce::jlimit(1, inCap, req > 0 ? req : 256);
        const double f0 = fed / expectedIn, f1 = (fed + req) / expectedIn;
        for (int ch = 0; ch < rc; ++ch)
            rtScratchIn.clear(ch, 0, req);
        const bool stillPlaying = dragscan::renderBlock(rtScratchIn, rc, req, docBuf, pos,
                                                        edgeAt(f0, startA, startB), edgeAt(f0, endA, endB),
                                                        edgeAt(f1, startA, startB), edgeAt(f1, endA, endB),
                                                        loop, maxFadeLen, currentSampleRate);
        fed += req;
        const float* ip[2] = { rtScratchIn.getReadPointer(0),
                               rtScratchIn.getReadPointer(rc > 1 ? 1 : 0) };
        rtStretcher->process(ip, (size_t) req, ! stillPlaying);
        if (! stillPlaying)
            rtFinished = true;
    }
}

// Scrub tool: a linear-interpolated, variable-rate (and reversible) read of the stored
// audio, at whatever rate WaveformDisplay's drag handling last computed from mouse
// velocity. No RubberBand involved -- pitch rising/falling with speed, and playing
// backwards cleanly at a negative rate, is exactly the point (a physical tape or turntable
// does the same); this is a much simpler DSP path than the pitch-corrected knobs.
void R3WRKAudioProcessor::renderScrub(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                      const juce::AudioBuffer<float>& docBuf)
{
    const int64_t docLen = docBuf.getNumSamples();
    if (docLen <= 1 || docBuf.getNumChannels() <= 0)
        return;

    // Defence in depth -- WaveformDisplay already clamps, but a stale or wild value here
    // would otherwise be audible as a shriek or a runaway read position.
    constexpr double maxSamplesPerSec = 44100.0 * 12.0;
    const double velocity = juce::jlimit(-maxSamplesPerSec, maxSamplesPerSec,
                                         document.scrubVelocity.load(std::memory_order_relaxed));
    const double perSample = velocity / juce::jmax(1.0, currentSampleRate);

    double pos = scrubReadPos;
    for (int i = 0; i < numSamples; ++i)
    {
        if (pos >= 0.0 && pos < (double) (docLen - 1))
        {
            const int64_t i0 = (int64_t) pos;
            const float frac = (float) (pos - (double) i0);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const int srcCh = juce::jmin(ch, docBuf.getNumChannels() - 1);
                const float* d = docBuf.getReadPointer(srcCh);
                out.setSample(ch, i, d[i0] + (d[i0 + 1] - d[i0]) * frac);
            }
        }
        // else: past either end -- leave this sample silent (buffer is already cleared)
        pos += perSample;
    }

    // Clamp so the cursor doesn't run away to +/-infinity while scrubbing off one end with
    // the mouse still held and moving.
    scrubReadPos = juce::jlimit(0.0, (double) (docLen - 1), pos);
    document.playhead.store((int64_t) scrubReadPos, std::memory_order_relaxed);
}

// See dragScanPos's header comment for the reasoning, and DragScanRender.h for the renderer
// itself (shared with the smoke test). `pos` never jumps here -- it either closes in on the
// region start at a capped, distance-proportional speed (still behind the window), or advances
// at plain 1x and wraps within the region (already inside it), with a loop-edge crossfade.
void R3WRKAudioProcessor::renderDragScan(juce::AudioBuffer<float>& out, int numCh, int numSamples,
                                         const juce::AudioBuffer<float>& docBuf, double& pos,
                                         double startA, double endA, double startB, double endB,
                                         bool loop, double maxFadeLen)
{
    if (! dragscan::renderBlock(out, numCh, numSamples, docBuf, pos,
                                startA, endA, startB, endB, loop, maxFadeLen, currentSampleRate))
        document.isPlaying.store(false, std::memory_order_relaxed);   // rest of the block stays silent
}

void R3WRKAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    // Black Box: snapshot the raw input now, before anything below can touch `buffer` --
    // appended for the branches where the input genuinely passes through (idle, recording); the
    // playback/scrub branches append their own rendered `buffer` instead, right where
    // captureOutput() does, since R3WRK has taken the input over for those blocks (see
    // blackBoxInputScratch's header comment).
    if (blackBoxCapacity > 0)
    {
        blackBoxInputScratch.setSize(numCh, numSamples, false, false, true);
        for (int ch = 0; ch < numCh; ++ch)
            blackBoxInputScratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    }

    // Black Box preview (Play in the popup): takes over the output entirely, same as the
    // desktop-recording bypass just below -- see startBlackBoxPreview()'s header comment.
    if (blackBoxPreviewPlaying.load(std::memory_order_relaxed))
    {
        renderBlackBoxPreview(buffer, numCh, numSamples);
        if (blackBoxCapacity > 0)
            appendToBlackBox(blackBoxInputScratch, numCh, numSamples);
        wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;
        return;
    }

    if (desktopRecording.load(std::memory_order_relaxed))
    {
        // ScreenCaptureKit is doing the capture on its own queue (appendDesktopSamples) --
        // nothing here to record or monitor.
        buffer.clear();
        if (blackBoxCapacity > 0)
            appendToBlackBox(blackBoxInputScratch, numCh, numSamples);
        wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;
        return;
    }

    if (document.isRecording.load(std::memory_order_relaxed))
    {
        ensureRecordingCapacity(numCh, numSamples);
        for (int ch = 0; ch < numCh; ++ch)
            recordingAccumulator.copyFrom(ch, (int) recordingWritePos, buffer, ch, 0, numSamples);
        recordingWritePos += numSamples;

        // Feed the live scope: one peak min/max per 256-sample hop of the incoming block.
        constexpr int hop = 256;
        for (int s = 0; s < numSamples; s += hop)
        {
            const int nn = juce::jmin(hop, numSamples - s);
            float mn = 0.0f, mx = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch) + s, nn);
                mn = juce::jmin(mn, r.getStart());
                mx = juce::jmax(mx, r.getEnd());
            }
            const int wpos = document.scopeWritePos.load(std::memory_order_relaxed);
            document.scopeMin[wpos] = mn;
            document.scopeMax[wpos] = mx;
            document.scopeWritePos.store((wpos + 1) % AudioDocument::scopeSize, std::memory_order_release);
        }
        document.recordedSamples.store(recordingWritePos, std::memory_order_relaxed);

        if (blackBoxCapacity > 0)
            appendToBlackBox(blackBoxInputScratch, numCh, numSamples);
        wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;
        return; // pass input through unchanged so the user can monitor while recording
    }

    if (document.isScrubbing.load(std::memory_order_relaxed))
    {
        buffer.clear();

        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (stl.isLocked())
        {
            if (! wasScrubbing)   // just started -- pick up from wherever the drag began
            {
                scrubReadPos = (double) document.playhead.load(std::memory_order_relaxed);
                scrubStopFadeRemaining = 0;   // a rapid re-press cancels any pending stop-fade
            }
            renderScrub(buffer, numCh, numSamples, document.getBuffer());
        }

        if (document.scrubStopRequested.exchange(false, std::memory_order_relaxed)
            && scrubStopFadeRemaining <= 0)
        {
            scrubStopFadeLen = (int) juce::jlimit<int64_t>(1, 512,
                (int64_t) (0.008 * currentSampleRate));
            scrubStopFadeRemaining = scrubStopFadeLen;
        }

        if (scrubStopFadeRemaining > 0)
        {
            const int n = juce::jmin(scrubStopFadeRemaining, numSamples);
            const int done = scrubStopFadeLen - scrubStopFadeRemaining;
            for (int i = 0; i < n; ++i)
            {
                // Equal-power fade-OUT: complementary curve to the fade-in used elsewhere
                // (cos ramps 1 -> 0 over the same x in [0,1] that sin ramps 0 -> 1 there).
                const double x = juce::jlimit(0.0, 1.0, (double) (done + i) / (double) scrubStopFadeLen);
                const double c = std::cos(0.5 * juce::MathConstants<double>::pi * x);
                const float g = (float) (c * c);
                for (int ch = 0; ch < numCh; ++ch)
                    buffer.setSample(ch, i, buffer.getSample(ch, i) * g);
            }
            scrubStopFadeRemaining -= n;
            if (scrubStopFadeRemaining <= 0)
            {
                document.isScrubbing = false;
                document.scrubVelocity = 0.0;
            }
        }

        // Filter + FX drawer ride scrub monitoring too, same as ordinary playback -- scrubbing
        // through a filtered/delayed/reverberated region should sound like that region, not like
        // the dry, unprocessed file. Only LFO modulation is deliberately excluded (see
        // applyPlaybackGain's call below): applyPlaybackFilter() is the plain non-modulated path,
        // never applyModulatedFilter(). freshPlayPass = a fresh scrub start (! wasScrubbing),
        // mirroring how the isPlaying branch primes these on ! wasPlaying.
        applyPlaybackFilter(buffer, numCh, 0, numSamples, ! wasScrubbing);
        applyPlaybackGain(buffer, numCh, 0, numSamples, {});   // Gain knob rides scrub monitoring too; LFOs don't run while scrubbing
        applyMimeophon(buffer, numCh, numSamples, ! wasScrubbing);
        applyReverb(buffer, numCh, numSamples, ! wasScrubbing);
        applyPlexiphon(buffer, numCh, numSamples, ! wasScrubbing);
        captureOutput(buffer, numCh, numSamples);
        // R3WRK has taken the buffer over to scrub -- Black Box follows that, not the (now
        // irrelevant) input snapshot; see blackBoxInputScratch's header comment.
        if (blackBoxCapacity > 0)
            appendToBlackBox(buffer, numCh, numSamples);

        wasScrubbing = true;
        wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;   // so normal playback resets the stretcher cleanly if it resumes
        return;
    }
    wasScrubbing = false;

    if (document.isPlaying.load(std::memory_order_relaxed))
    {
        buffer.clear();

        if (! wasPlaying)
        {
            // Every fresh play pass starts forward, except a reverse loop, which by
            // definition always plays backward.
            const bool startReversed = document.loopEnabled.load(std::memory_order_relaxed)
                                      && document.loopReverse.load(std::memory_order_relaxed)
                                      && ! document.loopPingPong.load(std::memory_order_relaxed);
            playbackDir = startReversed ? -1 : 1;
        }

        const double speed   = document.playbackSpeed.load(std::memory_order_relaxed);
        const double pitch   = document.playbackPitch.load(std::memory_order_relaxed);
        const double stretch = document.playbackStretch.load(std::memory_order_relaxed);
        const bool engaged = knobsEngaged(speed, pitch, stretch);

        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (stl.isLocked())
        {
            auto& docBuf = document.getBuffer();
            const int64_t docLen = document.getNumSamples();
            const bool loop       = document.loopEnabled.load(std::memory_order_relaxed);
            const bool pingPongOn = document.loopPingPong.load(std::memory_order_relaxed);
            const bool reverseLoopFlag = document.loopReverse.load(std::memory_order_relaxed);

            // Playback region: the selection if there is one (so Play plays the selected
            // range), otherwise the loop points, otherwise the whole clip. Loop loops it.
            // One getSelection() call so start+end always come from the same setSelection()
            // (they're packed into a single atomic on AudioDocument for exactly this reason).
            const auto sel  = document.getSelection();
            const int64_t selS = juce::jlimit((int64_t) 0, docLen, sel.getStart());
            const int64_t selE = juce::jlimit((int64_t) 0, docLen, sel.getEnd());
            const int64_t loopStartS = document.loopStart.load(std::memory_order_relaxed);
            const int64_t loopEndS   = document.loopEnd.load(std::memory_order_relaxed);

            int64_t rawRegionStart = 0, rawRegionEnd = docLen;
            if (selE > selS)                              { rawRegionStart = selS;       rawRegionEnd = selE; }
            else if (loop && loopEndS > loopStartS)       { rawRegionStart = loopStartS; rawRegionEnd = loopEndS; }

            // A Start/End knob or waveform bracket is being dragged: setSelection() fires far
            // faster than a hard-snapped playhead can gracefully follow, which used to be an
            // audible click storm (region-jump-per-block) and then, briefly, a full mute or a
            // synthetic tape-wind chase. Neither kept the thing the user actually wanted: the
            // *real* loop genuinely playing while it scans across the file. So instead, slew
            // the region edges themselves toward their live values at a bounded, distance-
            // proportional rate (same shape as the loop-crossfade/declick envelopes elsewhere)
            // -- the window that's actually looping/playing below then scans smoothly across
            // the file rather than teleporting, with the ordinary machinery (loop wrap,
            // crossfade, ping-pong, RubberBand if engaged) still genuinely rendering it the
            // whole time. `dragRegionSeeded` means "these are live", not "the file's default
            // 0..docLen" -- without it the very first dragging block would slew from the wrong
            // starting point.
            const int dragEdge = document.selectionEdgeDragging.load(std::memory_order_relaxed);
            constexpr double slewGainPerSec = 8.0;   // catch-up rate = distance * this
            // Must match renderDragScan's own internal catch-up gain -- kept as a literal there
            // too (not plumbed through as a parameter) so the two constants can't drift apart
            // silently; see the comment below on why raising *that* one instead of this cap was
            // tried and made things worse.
            constexpr double catchUpGainPerSec = 8.0;
            // This cap is also, in effect, the forward-drag pitch-shift cap: renderDragScan's
            // playhead has to keep pace with however fast this window is moving in order to
            // stay caught up, and (unlike a backward drag, which never needs to chase at all
            // -- see its comment) a forward drag runs at close to this speed continuously
            // for as long as the mouse keeps moving forward, not just as a brief catch-up.
            // Kept modest (rather than the ~6x this used to be) so that's a mild lift instead
            // of "extreme pitch" -- renderDragScan's own cap is set a little above this one,
            // so the gap can still actually close.
            const double baseMaxSlewSpeed = currentSampleRate * 1.5;   // cap: ~1.5x normal speed
            // A loop shorter than baseMaxSlewSpeed/catchUpGainPerSec (~187ms at the stock
            // constants) can never actually be re-entered during a sustained drag: the window
            // keeps outrunning the catch-up forever, leaving the playhead permanently trailing
            // outside it, reading unrelated material further back in the file at a pitched-up
            // rate -- confirmed by simulation, this was the "distortion on a small selection"
            // bug. First attempt raised catchUpGainPerSec itself to compensate, which made things
            // *worse*: a much higher proportional gain turns the gentle, already-proven catch-up
            // response into a near-bang-bang one that amplifies ordinary drag-input jitter into
            // audible chatter, instead of just closing the gap. Throttling the window's own top
            // speed for a short loop leaves that catch-up response completely untouched --
            // it only creeps slower, which is exactly the "slowly steps toward the new loop"
            // behavior already confirmed to feel right for big loops, just scaled down to fit a
            // small one. Based on the *target* length (rawRegionEnd-rawRegionStart), known before
            // slewing, so it doesn't chase a moving figure while the loop itself resizes.
            const double targetRegionLen = (double) juce::jmax((int64_t) 1, rawRegionEnd - rawRegionStart);
            constexpr double maxTrailingFractionOfLoop = 0.35;
            const double maxSlewSpeed = juce::jlimit(currentSampleRate * 0.1, baseMaxSlewSpeed,
                targetRegionLen * catchUpGainPerSec * maxTrailingFractionOfLoop);
            int64_t regionStart, regionEnd;
            double prevDragRegionStart = 0.0, prevDragRegionEnd = 0.0;   // edges at this block's start
            if (dragEdge != 0)
            {
                if (! dragRegionSeeded)
                {
                    dragRegionStart = (double) rawRegionStart;
                    dragRegionEnd   = (double) rawRegionEnd;
                    dragRegionSeeded = true;
                }
                prevDragRegionStart = dragRegionStart;
                prevDragRegionEnd   = dragRegionEnd;
                const double dt = (double) numSamples / juce::jmax(1.0, currentSampleRate);
                auto slew = [&](double& smoothed, int64_t target)
                {
                    const double distance = (double) target - smoothed;
                    const double vel = juce::jlimit(-maxSlewSpeed, maxSlewSpeed, distance * slewGainPerSec);
                    smoothed += vel * dt;
                };
                slew(dragRegionStart, rawRegionStart);
                slew(dragRegionEnd,   rawRegionEnd);
                regionStart = (int64_t) std::llround(dragRegionStart);
                regionEnd   = juce::jmax(regionStart + 1, (int64_t) std::llround(dragRegionEnd));
            }
            else
            {
                dragRegionSeeded = false;   // next drag starts fresh from wherever the region is
                regionStart = rawRegionStart;
                regionEnd   = rawRegionEnd;
            }

            // Ping-pong needs at least 3 frames to have a distinct return leg; below that it
            // just plays as a plain loop.
            const bool pingPong = loop && pingPongOn && (regionEnd - regionStart) >= 3;
            // Mutually exclusive with pingPong (the Loop button cycles through one or the
            // other, never both); no minimum-length guard like ping-pong's -- a reverse loop
            // only ever runs one direction, so it has no distinct "return leg" to need one.
            const bool reverseLoopOn = loop && reverseLoopFlag && ! pingPong;

            // Loop crossfade: raised-cosine volume envelope over the first/last N ms of the
            // loop region so the wrap doesn't click. Loop only; clamped to half the region.
            const double xfadeMs = document.loopCrossfadeMs.load(std::memory_order_relaxed);
            const int loopFadeLen = (loop && xfadeMs > 0.01)
                ? (int) juce::jmin<int64_t>((int64_t) (xfadeMs * currentSampleRate / 1000.0),
                                            (regionEnd - regionStart) / 2)
                : 0;

            if (dragEdge != 0)
            {
                // Dragging, plain path: a continuous fractional read position instead of the
                // ordinary integer pos below -- see renderDragScan's header comment for why.
                // Position itself never jumps at the hand-off (dragScanPos is seeded from the
                // exact live playhead below), but renderDragScan always reads *forward* --  if
                // the ordinary path happened to be on a ping-pong loop's backward return leg
                // (playbackDir == -1) at the exact instant you grab a bracket/knob, direction
                // reverses right at the hand-off even though position lines up, which is
                // audible as a click on the material itself. Crossfade it out exactly like a
                // manual seek's old tail (same fields, same ramp shape, same application code
                // below), just captured playing in whatever direction was actually live.
                if (! dragScanActive && ! engaged)
                {
                    declickLen = (int) juce::jlimit<int64_t>(1, 512,
                        (int64_t) (0.008 * currentSampleRate));
                    seekOldTail.setSize(numCh, declickLen, false, false, true);
                    const int64_t oldDocLen = docBuf.getNumSamples();
                    const int oldChans = docBuf.getNumChannels();
                    for (int i = 0; i < declickLen; ++i)
                    {
                        const int64_t s = lastKnownPlayhead + (int64_t) i * playbackDir;
                        const bool valid = s >= 0 && s < oldDocLen && oldChans > 0;
                        for (int ch = 0; ch < numCh; ++ch)
                            seekOldTail.setSample(ch, i, valid
                                ? docBuf.getSample(juce::jmin(ch, oldChans - 1), (int) s) : 0.0f);
                    }
                    seekCrossfadeActive = true;
                    declickRemaining = declickLen;
                }
                if (! dragScanActive)
                {
                    // Stretched: the playhead is RubberBand's input read position, so seeding
                    // from it keeps the input continuous -- no crossfade needed on the way in.
                    dragScanPos = (double) document.playhead.load(std::memory_order_relaxed);
                    dragScanActive = true;
                }
                const double dragFadeLen = (loop && xfadeMs > 0.01) ? xfadeMs * currentSampleRate / 1000.0 : 0.0;
                if (engaged)
                {
                    if (rtStretcher != nullptr && (! wasPlaying || ! stretcherPrimed))
                    {
                        rtStretcher->reset();
                        stretcherPrimed = true;
                        rtFinished = false;
                        stretchRatioNeedsSnap = true;
                    }
                    renderDragScanStretched(buffer, numCh, numSamples, docBuf, dragScanPos,
                                            prevDragRegionStart, prevDragRegionEnd, dragRegionStart, dragRegionEnd,
                                            loop, dragFadeLen, speed, pitch, stretch);
                }
                else
                {
                    stretcherPrimed = false;
                    rtFinished = false;
                    renderDragScan(buffer, numCh, numSamples, docBuf, dragScanPos,
                                   prevDragRegionStart, prevDragRegionEnd, dragRegionStart, dragRegionEnd,
                                   loop, dragFadeLen);
                }
                document.playhead.store((int64_t) std::llround(dragScanPos), std::memory_order_relaxed);
            }
            else
            {
                bool releasedFromDrag = false, releasedIntoStretcher = false;
                if (dragScanActive)
                {
                    // Drag just ended (or crossed into the RubberBand-engaged case) -- hand off
                    // to the ordinary path from wherever the scan landed. The region snaps from
                    // its slewed position to where the mouse actually left it, so the playhead
                    // can end up outside it (jumped to the loop start just below) or with the
                    // loop edges moved under it -- either way a splice. Crossfade out what the
                    // drag renderer would have played next (see renderReleaseTail), exactly like
                    // a manual seek's old tail, so the release is a quick blend, not a click.
                    declickLen = (int) juce::jlimit<int64_t>(1, 512,
                        (int64_t) (0.008 * currentSampleRate));
                    const double relFade = (loop && xfadeMs > 0.01) ? xfadeMs * currentSampleRate / 1000.0 : 0.0;
                    const double relEnd = juce::jmax(dragRegionStart + 1.0, dragRegionEnd);
                    if (engaged)
                    {
                        // Stretched: blend on RubberBand's input instead (releaseInputTail).
                        dragscan::renderReleaseTail(releaseInputTail, rtChannels, declickLen, docBuf, dragScanPos,
                                                    dragRegionStart, relEnd, loop, relFade, currentSampleRate);
                        releaseInputTailLen = releaseInputTailRemaining = declickLen;
                        releasedIntoStretcher = true;
                    }
                    else
                    {
                        dragscan::renderReleaseTail(seekOldTail, numCh, declickLen, docBuf, dragScanPos,
                                                    dragRegionStart, relEnd, loop, relFade, currentSampleRate);
                        seekCrossfadeActive = true;
                        declickRemaining = declickLen;
                    }
                    releasedFromDrag = true;

                    document.playhead.store((int64_t) std::llround(dragScanPos), std::memory_order_relaxed);
                    dragScanActive = false;
                }

                int64_t pos = document.playhead.load(std::memory_order_relaxed);
                // Read-and-clear: a manual seek (click-to-seek, or a marker-less slice pick)
                // that happens to land inside a region that isn't itself changing -- see
                // declickRequested's comment on AudioDocument for why the check just below
                // can't notice that jump on its own.
                const bool manualSeek = document.declickRequested.exchange(false, std::memory_order_relaxed);
                // A reverse loop's valid backward-start range is the mirror image of the
                // ordinary forward one: regionEnd itself is a valid position to begin
                // reading backward from, and regionStart is the one that's now out of range.
                const bool posOutOfRegion = reverseLoopOn ? (pos <= regionStart || pos > regionEnd)
                                                           : (pos <  regionStart || pos >= regionEnd);
                if (posOutOfRegion)
                {
                    // Snap a stray playhead into the region -- but ONLY when there's actually
                    // somewhere to keep playing (loop/ping-pong/reverse). `pos >= regionEnd` is
                    // true not just for a genuinely stray position (e.g. a document edit shrank
                    // the buffer) but for the ordinary, expected case of a non-looping pass
                    // simply reaching its natural end -- which, thanks to block-aligned reads,
                    // happens to land pos exactly ON regionEnd almost every time. This used to
                    // unconditionally treat that as "stray" and snap back to regionStart with
                    // playbackDir=1, restarting playback with loop supposedly off -- and, for
                    // ping-pong specifically, it always did a plain forward wrap here instead of
                    // gatherRegion's own backward reflection, so ping-pong could never actually
                    // bounce once its cycle length happened to land on a block boundary.
                    if (reverseLoopOn)
                    {
                        pos = regionEnd;           // still playing backward, re-enter at the tail
                        playbackDir = -1;
                    }
                    else if (pingPong && pos >= regionEnd)
                    {
                        // Mirrors gatherRegion's own forward-hits-end reflection.
                        pos = juce::jmax(regionStart, regionEnd - 2);
                        playbackDir = -1;
                    }
                    else if (loop)
                    {
                        pos = regionStart;
                        playbackDir = 1;
                    }
                    else
                    {
                        // Not looping in any mode: don't snap-and-restart. Just clamp into range
                        // so the ordinary render call below (gatherRegion, via renderPlayback-
                        // Direct/Stretched) sees a safe, in-bounds position and takes its own
                        // "reached the end, loop is off -> stop" path, exactly as it already does
                        // whenever this recovery snap doesn't happen to preempt it.
                        pos = juce::jlimit(regionStart, regionEnd, pos);
                    }

                    // A genuine snap-and-continue jump (the three branches above that keep
                    // playing -- all imply loop, see their own computation) is an arbitrary
                    // splice in the waveform -- ramp in over a few ms so it's a soft thump
                    // instead of a pop (see the declick fields' comment). Skipped for the
                    // not-looping clamp above: nothing is jumping there, gatherRegion below just
                    // finds the natural end and stops.
                    if (loop && ! releasedIntoStretcher)   // stretched release blends on the input instead
                    {
                        declickLen = (int) juce::jlimit<int64_t>(1, 512,
                            (int64_t) (0.008 * currentSampleRate));
                        declickRemaining = declickLen;
                        // No coherent "old" material for this kind of jump -- except right after
                        // a drag release, which just captured the drag renderer's own tail.
                        if (! releasedFromDrag)
                            seekCrossfadeActive = false;
                    }
                }
                else if (manualSeek)
                {
                    declickLen = (int) juce::jlimit<int64_t>(1, 512,
                        (int64_t) (0.008 * currentSampleRate));
                    declickRemaining = declickLen;

                    // Also crossfade out whatever was actually playing a moment ago (see
                    // seekOldTail's comment) -- the ramp above only softens the incoming edge;
                    // the outgoing one, where the old material just stops, clicks on its own
                    // otherwise. Captured now, while the lock is held and docBuf is reachable,
                    // so the declick-application code below (which runs unlocked) doesn't need it.
                    seekOldTail.setSize(numCh, declickLen, false, false, true);
                    const int64_t oldDocLen = docBuf.getNumSamples();
                    const int oldChans = docBuf.getNumChannels();
                    for (int i = 0; i < declickLen; ++i)
                    {
                        const int64_t s = lastKnownPlayhead + i;
                        const bool valid = s >= 0 && s < oldDocLen && oldChans > 0;
                        for (int ch = 0; ch < numCh; ++ch)
                            seekOldTail.setSample(ch, i, valid
                                ? docBuf.getSample(juce::jmin(ch, oldChans - 1), (int) s) : 0.0f);
                    }
                    seekCrossfadeActive = true;
                }

                // Reset the stretcher at the start of a play pass, or when the knobs cross the
                // bypass/engaged line, so no stale tail leaks in.
                if (engaged)
                {
                    if (rtStretcher != nullptr && (! wasPlaying || ! stretcherPrimed))
                    {
                        rtStretcher->reset();
                        stretcherPrimed = true;
                        rtFinished = false;
                        stretchRatioNeedsSnap = true;   // start at the current ratio, no 120ms slide in
                    }
                    renderPlaybackStretched(buffer, numCh, numSamples, docBuf, pos,
                                            playbackDir, regionStart, regionEnd, loop, pingPong, reverseLoopOn, loopFadeLen,
                                            speed, pitch, stretch);
                }
                else
                {
                    stretcherPrimed = false;
                    rtFinished = false;
                    renderPlaybackDirect(buffer, numCh, numSamples, docBuf, pos,
                                         playbackDir, regionStart, regionEnd, loop, pingPong, reverseLoopOn, loopFadeLen);
                }
            }
        }

        // Our own record of where playback last was, kept independent of document.playhead
        // itself so a manual seek overwriting that atomic doesn't erase what "old" means -- see
        // lastKnownPlayhead's comment.
        lastKnownPlayhead = document.playhead.load(std::memory_order_relaxed);

        // Declick ramp-in after a region-jump snap (see declickRemaining's comment) --
        // equal-power raised-cosine, same shape as loopFadeGain, just applied to the whole
        // block's output rather than one region edge. A manual seek's ramp also crossfades out
        // seekOldTail (captured above, while docBuf was reachable) so the material the old
        // playhead left behind fades out instead of just stopping -- see its comment.
        if (declickRemaining > 0)
        {
            const int n = juce::jmin(declickRemaining, numSamples);
            const int done = declickLen - declickRemaining;
            for (int i = 0; i < n; ++i)
            {
                const double x = juce::jlimit(0.0, 1.0, (double) (done + i) / (double) declickLen);
                const double s = std::sin(0.5 * juce::MathConstants<double>::pi * x);
                const float g = (float) (s * s);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    const float in = buffer.getSample(ch, i) * g;
                    // seekOldTail is indexed globally across the whole ramp (like `x` above),
                    // not per-block, since the ramp -- and so the tail -- can span more than
                    // one block.
                    const float old = seekCrossfadeActive ? seekOldTail.getSample(ch, done + i) * (1.0f - g) : 0.0f;
                    buffer.setSample(ch, i, in + old);
                }
            }
            declickRemaining -= n;
            if (declickRemaining <= 0)
                seekCrossfadeActive = false;
        }

        // Amplify panel audition (see AudioDocument::previewGainLinear's comment): while the
        // panel's slider is being dragged, hear the gain change on whatever's currently
        // playing -- normally the selection, looped for the panel's lifetime by AmplifyPanel
        // itself so there's always something to audition -- not just see it in the waveform.
        // Applied to the raw playback before the filter/output gain, same position
        // EditActions::applyGainDb's real (destructive) gain sits in the chain conceptually,
        // so the audition matches what Apply would actually bake in.
        if (document.previewActive.load(std::memory_order_relaxed))
        {
            const float previewGain = document.previewGainLinear.load(std::memory_order_relaxed);
            for (int ch = 0; ch < numCh; ++ch)
                buffer.applyGain(ch, 0, numSamples, previewGain);
        }

        // Multi-mode filter, last in the chain -- applied whether the try-lock was held or not
        // (a filtered near-silent block is still correct), and to both direct + stretched paths.
        //
        // Two different filter paths, chosen once per block: ordinary knob-driven filtering runs
        // through the existing direct-form biquad (applyPlaybackFilter(), chunked below); an
        // actively LFO-modulated filter runs through applyModulatedFilter() instead -- a TPT
        // state-variable filter, ticked and recomputed truly per-sample for the whole block, in
        // ModulatedMultiModeFilter (BiquadFilter.h). The direct-form biquad isn't built to have
        // its coefficients changed continuously and fast (verified offline: it produces non-
        // finite output under exactly this kind of stress -- see the LFO-modulated filter
        // section of Tests/SmokeTest.cpp); the TPT filter is. applyModulatedFilter() scans for
        // an active filter-targeting LFO itself and returns whether it handled the block, so
        // whichever path ran, the other is skipped entirely -- no double filtering.
        //
        // Every LFO's phase resets here, once, on a fresh play pass -- before
        // applyModulatedFilter() (which ticks filter-targeting slots itself, per-sample) or
        // tickLfos() (Gain-targeting slots, chunked) touch any of them, so neither path can ever
        // tick from a stale leftover phase on the first block of a new play pass.
        if (! wasPlaying)
        {
            const int n = numActiveLfoSlots();
            for (int i = 0; i < n; ++i)
                lfoDsp[i].reset();
        }
        const bool filterLfoActive = applyModulatedFilter(buffer, numCh, numSamples, ! wasPlaying);

        // Ticked/applied in fixed-size sub-chunks, not once for the whole (host-chosen) block:
        // an LFO can swing all the way across its range within a single large host block, and
        // updating the filter/gain coefficients only once for that whole span turns a sweep into
        // an audible staircase (heard as crackle, most noticeably modulating Filter Width) --
        // see kLfoModUpdateSamples's comment. freshPlayPass only applies to the very first chunk;
        // later chunks in the same processBlock() call must not re-trigger that reset.
        int lfoModDone = 0;
        while (lfoModDone < numSamples)
        {
            const int chunk = juce::jmin(kLfoModUpdateSamples, numSamples - lfoModDone);
            const bool freshPlayPass = (! wasPlaying) && lfoModDone == 0;
            const auto lfoMod = tickLfos(chunk);
            if (! filterLfoActive)
                applyPlaybackFilter(buffer, numCh, lfoModDone, chunk, freshPlayPass);
            applyPlaybackGain(buffer, numCh, lfoModDone, chunk, lfoMod);
            lfoModDone += chunk;
        }

        // Mimeophon, after filter and gain, before Reverb/Plexiphon -- a conventional "delay
        // before reverb" chain position, and matches the FX drawer's own left-to-right slot
        // order (Delay is the leftmost slot). Unchunked: not LFO-modulated in phase 1.
        applyMimeophon(buffer, numCh, numSamples, ! wasPlaying);

        // Reverb, after filter/gain/Mimeophon -- like a send on the end of the strip. Unchunked:
        // not LFO-modulated in phase 1, so nothing here needs kLfoModUpdateSamples's finer
        // update rate. freshPlayPass primes it the same way the filter/gain above do.
        applyReverb(buffer, numCh, numSamples, ! wasPlaying);

        // Plexiphon, after Reverb -- an arbitrary but reasonable "read the FX drawer left to
        // right" chain order (LFO | Delay | Reverb | Plexiphon), not a hard requirement.
        applyPlexiphon(buffer, numCh, numSamples, ! wasPlaying);

        captureOutput(buffer, numCh, numSamples);
        // R3WRK has taken the buffer over to play/loop -- Black Box follows that, not the (now
        // irrelevant) input snapshot; see blackBoxInputScratch's header comment.
        if (blackBoxCapacity > 0)
            appendToBlackBox(buffer, numCh, numSamples);

        wasPlaying = true;
        return;
    }

    const bool justStoppedPlaying = wasPlaying;
    wasPlaying = false; declickRemaining = 0; releaseInputTailRemaining = 0; dragRegionSeeded = false; dragScanActive = false;

    // A reverb tail outlives the signal that made it -- start a countdown the instant playback
    // stops (if the reverb was actually engaged), so applyReverb() keeps ticking with silence as
    // its "input" for a while after, letting only the already-recirculating tail ring out instead
    // of cutting it off dead. The ceiling here is generous (5 minutes), not a real prediction of
    // tail length -- near-max Decay is *designed* for near-infinite sustain (matches the real
    // hardware); what actually ends it at a normal, finite Decay setting is the energy check
    // below (reverbTailSilentSamples), once the tail's genuinely gone quiet.
    if (justStoppedPlaying)
    {
        const bool reverbEngaged = document.reverbEnabled.load(std::memory_order_relaxed)
                                    && document.reverbMix.load(std::memory_order_relaxed) > 0.001;
        reverbTailSamplesLeft = reverbEngaged ? (int) (currentSampleRate * 300.0) : 0;
        reverbTailSilentSamples = 0;

        const bool plexEngaged = document.plexEnabled.load(std::memory_order_relaxed)
                                 && document.plexMix.load(std::memory_order_relaxed) > 0.001;
        plexTailSamplesLeft = plexEngaged ? (int) (currentSampleRate * 300.0) : 0;
        plexTailSilentSamples = 0;

        const bool mimeoEngaged = document.mimeoEnabled.load(std::memory_order_relaxed)
                                  && document.mimeoMix.load(std::memory_order_relaxed) > 0.001;
        mimeoTailSamplesLeft = mimeoEngaged ? (int) (currentSampleRate * 300.0) : 0;
        mimeoTailSilentSamples = 0;
    }

    // Neither recording nor playing back: leave `buffer` untouched so the host's input
    // passes straight through -- except Auto-Record standby, which watches that same
    // pass-through input for a peak loud enough to cross autoRecordThresholdDb. Read-only:
    // it never touches `buffer` or starts recording itself, just flags it for the message
    // thread (see EditorToolbar::timerCallback) to act on -- see AudioDocument's comment.

    // Live input monitor: feed a scope ring from the still-pristine input -- the host's input in
    // VST/AU, whatever's selected in Audio Settings in Standalone -- so WaveformDisplay can draw
    // a live oscilloscope while genuinely idle. Deliberately placed before the reverb/plex/mimeo
    // tail-ringout blocks below, which additively mutate `buffer` while a decay tail is still
    // ringing out -- reading here instead means the monitor always shows the real incoming
    // signal, not a stale effect tail standing in for it.
    //
    // A much finer hop than the recording scope's own 256 (which, over monitorScopeSize's 1024
    // slots, would span ~6s squeezed across the whole width -- too zoomed out to read as a
    // waveform at all, more a low-res level meter). 6 samples/hop * 1024 slots is ~140ms of
    // audio at 44.1kHz filling the same width -- a tight scope-sweep view.
    {
        constexpr int hop = 6;
        for (int s = 0; s < numSamples; s += hop)
        {
            const int nn = juce::jmin(hop, numSamples - s);
            float mn = 0.0f, mx = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch) + s, nn);
                mn = juce::jmin(mn, r.getStart());
                mx = juce::jmax(mx, r.getEnd());
            }
            const int wpos = document.monitorScopeWritePos.load(std::memory_order_relaxed);
            document.monitorScopeMin[wpos] = mn;
            document.monitorScopeMax[wpos] = mx;
            document.monitorScopeWritePos.store((wpos + 1) % AudioDocument::monitorScopeSize,
                                                std::memory_order_release);
        }
    }

    if (document.autoRecordEnabled.load(std::memory_order_relaxed)
        && ! document.autoRecordTriggered.load(std::memory_order_relaxed))
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch), numSamples);
            peak = juce::jmax(peak, std::abs(r.getStart()), std::abs(r.getEnd()));
        }
        const float peakDb = juce::Decibels::gainToDecibels(peak, -100.0f);
        if (peakDb >= (float) document.autoRecordThresholdDb.load(std::memory_order_relaxed))
            document.autoRecordTriggered.store(true, std::memory_order_relaxed);
    }

    // Once a sample has been recorded/loaded, idle blocks go silent instead of passing the input
    // through -- live-input monitoring is only for the empty-document state (matching the
    // oscilloscope's own isEmpty() gate in WaveformDisplay::paint). Done after the monitor scope
    // and Auto-Record detection above, which both need the real input, and before the FX tail
    // ring-outs below, which add onto whatever's left in `buffer`. A missed try-lock means the
    // message thread is mid-edit on the document -- which only happens when there's something
    // there -- so it counts as non-empty.
    {
        const juce::CriticalSection::ScopedTryLockType stl(document.getLock());
        if (! stl.isLocked() || ! document.isEmpty())
            buffer.clear();
    }

    // Let a still-ringing reverb tail continue decaying over host passthrough (or silence in the
    // Standalone), additively -- NOT unconditionally on every idle block, which would mean
    // R3WRK reverberating any live signal passing through any time it isn't actively playing its
    // own buffer, a standing side effect on whatever a DAW channel happens to be carrying. Feeds
    // the engine silence (see applyReverb()'s tailOnly parameter), so only what's already
    // recirculating in its delay lines comes back out.
    if (reverbTailSamplesLeft > 0)
    {
        const int n = juce::jmin(reverbTailSamplesLeft, numSamples);
        const float wetPeak = applyReverb(buffer, numCh, n, false, true);
        reverbTailSamplesLeft -= n;

        // Ends the tail once it's genuinely inaudible, however long that takes -- rather than
        // waiting out the full (deliberately generous) ceiling above every time. A single quiet
        // moment doesn't end it early: the tail has to stay below threshold for a full 2 seconds
        // straight, so an ordinary lull mid-decay can't be mistaken for the tail being over.
        constexpr float kSilenceThreshold = 0.0005f;   // roughly -66dBFS, well under audible
        if (wetPeak > kSilenceThreshold)
            reverbTailSilentSamples = 0;
        else
            reverbTailSilentSamples += n;
        if (reverbTailSilentSamples > (int) (currentSampleRate * 2.0))
            reverbTailSamplesLeft = 0;
    }

    // Same idle tail-ring-out treatment for Plexiphon -- see the block above.
    if (plexTailSamplesLeft > 0)
    {
        const int n = juce::jmin(plexTailSamplesLeft, numSamples);
        const float wetPeak = applyPlexiphon(buffer, numCh, n, false, true);
        plexTailSamplesLeft -= n;

        constexpr float kSilenceThreshold = 0.0005f;
        if (wetPeak > kSilenceThreshold)
            plexTailSilentSamples = 0;
        else
            plexTailSilentSamples += n;
        if (plexTailSilentSamples > (int) (currentSampleRate * 2.0))
            plexTailSamplesLeft = 0;
    }

    // Same idle tail-ring-out treatment for Mimeophon -- see the reverb block above.
    if (mimeoTailSamplesLeft > 0)
    {
        const int n = juce::jmin(mimeoTailSamplesLeft, numSamples);
        const float wetPeak = applyMimeophon(buffer, numCh, n, false, true);
        mimeoTailSamplesLeft -= n;

        constexpr float kSilenceThreshold = 0.0005f;
        if (wetPeak > kSilenceThreshold)
            mimeoTailSilentSamples = 0;
        else
            mimeoTailSilentSamples += n;
        if (mimeoTailSilentSamples > (int) (currentSampleRate * 2.0))
            mimeoTailSamplesLeft = 0;
    }

    // Keep the captured timeline continuous through idle gaps (in the Standalone the input is
    // muted, so this is silence -- but it stops a stop/start of playback mid-capture from
    // splicing the two parts together with no gap).
    captureOutput(buffer, numCh, numSamples);

    // Genuinely idle: `buffer` above is untouched host input, exactly what blackBoxInputScratch
    // already snapshotted at the top of this function.
    if (blackBoxCapacity > 0)
        appendToBlackBox(blackBoxInputScratch, numCh, numSamples);
}

R3WRKAudioProcessor::LfoModResult R3WRKAudioProcessor::tickLfos(int numSamples)
{
    LfoModResult r;
    const int n = numActiveLfoSlots();

    // Every slot's phase is reset centrally in processBlock() on a fresh play pass, before this
    // or applyModulatedFilter() runs.
    for (int i = 0; i < n; ++i)
    {
        auto& slot = document.lfoSlots[i];
        if (! slot.enabled.load(std::memory_order_relaxed))
            continue;

        const auto target = (r3wrk::ModTarget) juce::jlimit(0, r3wrk::kNumModTargets - 1,
                                                             slot.target.load(std::memory_order_relaxed));

        // Filter targets (Base/Width/HP Q/LP Q) are ticked per-sample by applyModulatedFilter()
        // instead -- see its comment. Skip them here entirely, including the tickBlock() call
        // that advances lfoDsp[i]'s phase, so a filter-targeting slot's phase is only ever
        // advanced by one mechanism, never both (which would double-advance it).
        if (target != r3wrk::ModTarget::gain)
            continue;

        const auto shape = (r3wrk::LfoShape) juce::jlimit(0, (int) r3wrk::LfoShape::sampleHold,
                                                           slot.shape.load(std::memory_order_relaxed));
        const auto range = (r3wrk::LfoRateRange) juce::jlimit(0, (int) r3wrk::LfoRateRange::audio,
                                                               slot.rateRange.load(std::memory_order_relaxed));
        const double rate01 = juce::jlimit(0.0, 1.0, slot.rate01.load(std::memory_order_relaxed));
        const double amount = juce::jlimit(-1.0, 1.0, slot.amount.load(std::memory_order_relaxed));

        const double hz  = r3wrk::lfoRateHz(rate01, range);
        const double out = lfoDsp[i].tickBlock(hz, shape, currentSampleRate, numSamples);   // -1..+1
        r.gainDb += out * amount * kLfoGainModRangeDb;
    }
    return r;
}

namespace
{
    bool isFilterModTarget(r3wrk::ModTarget t)
    {
        return t == r3wrk::ModTarget::filterBase || t == r3wrk::ModTarget::filterWidth
            || t == r3wrk::ModTarget::filterHpQ  || t == r3wrk::ModTarget::filterLpQ;
    }
}

bool R3WRKAudioProcessor::applyModulatedFilter(juce::AudioBuffer<float>& buffer, int numCh, int numSamples, bool freshPlayPass)
{
    const int n = numActiveLfoSlots();

    bool anyActive = false;
    for (int i = 0; i < n; ++i)
    {
        auto& slot = document.lfoSlots[i];
        if (slot.enabled.load(std::memory_order_relaxed)
            && isFilterModTarget((r3wrk::ModTarget) juce::jlimit(0, r3wrk::kNumModTargets - 1,
                                                                  slot.target.load(std::memory_order_relaxed))))
        {
            anyActive = true;
            break;
        }
    }

    if (! anyActive)
    {
        // Wasn't active last block either -- nothing to clean up, common case.
        if (modulatedFilterWasActive)
        {
            modulatedFilter[0].reset();
            modulatedFilter[1].reset();
            modulatedFilterWasActive = false;
        }
        return false;
    }

    const double baseKnob  = juce::jlimit(0.0, 1.0, document.filterBase.load(std::memory_order_relaxed));
    const double widthKnob = juce::jlimit(0.0, 1.0, document.filterWidth.load(std::memory_order_relaxed));
    const double hpQKnob   = juce::jlimit(0.0, 1.0, document.filterHpQ.load(std::memory_order_relaxed));
    const double lpQKnob   = juce::jlimit(0.0, 1.0, document.filterLpQ.load(std::memory_order_relaxed));

    if (freshPlayPass || ! modulatedFilterWasActive)
    {
        modulatedFilter[0].reset();
        modulatedFilter[1].reset();
        smoothedFilterBase.setCurrentAndTargetValue(baseKnob);
        smoothedFilterWidth.setCurrentAndTargetValue(widthKnob);
        smoothedFilterHpQ.setCurrentAndTargetValue(hpQKnob);
        smoothedFilterLpQ.setCurrentAndTargetValue(lpQKnob);
    }
    modulatedFilterWasActive = true;

    const auto fm = (r3wrk::FilterModel) juce::jlimit(0, 1, document.filterModel.load(std::memory_order_relaxed));
    modulatedFilter[0].model = fm;
    modulatedFilter[1].model = fm;

    smoothedFilterBase.setTargetValue(baseKnob);
    smoothedFilterWidth.setTargetValue(widthKnob);
    smoothedFilterHpQ.setTargetValue(hpQKnob);
    smoothedFilterLpQ.setTargetValue(lpQKnob);

    for (int i = 0; i < numSamples; ++i)
    {
        // Every filter-targeting LFO is ticked exactly one sample here (not chunked -- see
        // kLfoModUpdateSamples's comment on why that doesn't work for this path), and their
        // offsets onto the same target simply add, same as two signals into one summing node.
        double offBase = 0.0, offWidth = 0.0, offHpQ = 0.0, offLpQ = 0.0;
        for (int s = 0; s < n; ++s)
        {
            auto& slot = document.lfoSlots[s];
            if (! slot.enabled.load(std::memory_order_relaxed))
                continue;
            const auto target = (r3wrk::ModTarget) juce::jlimit(0, r3wrk::kNumModTargets - 1,
                                                                 slot.target.load(std::memory_order_relaxed));
            if (! isFilterModTarget(target))
                continue;

            const auto shape = (r3wrk::LfoShape) juce::jlimit(0, (int) r3wrk::LfoShape::sampleHold,
                                                               slot.shape.load(std::memory_order_relaxed));
            const auto range = (r3wrk::LfoRateRange) juce::jlimit(0, (int) r3wrk::LfoRateRange::audio,
                                                                   slot.rateRange.load(std::memory_order_relaxed));
            const double rate01 = juce::jlimit(0.0, 1.0, slot.rate01.load(std::memory_order_relaxed));
            const double amount = juce::jlimit(-1.0, 1.0, slot.amount.load(std::memory_order_relaxed));

            const double hz     = r3wrk::lfoRateHz(rate01, range);
            const double out    = lfoDsp[s].tickBlock(hz, shape, currentSampleRate, 1);   // one sample
            const double offset = out * amount;

            switch (target)
            {
                case r3wrk::ModTarget::filterBase:  offBase  += offset; break;
                case r3wrk::ModTarget::filterWidth: offWidth += offset; break;
                case r3wrk::ModTarget::filterHpQ:   offHpQ   += offset; break;
                case r3wrk::ModTarget::filterLpQ:   offLpQ   += offset; break;
                default: break;
            }
        }

        const double b  = juce::jlimit(0.0, 1.0, smoothedFilterBase.getNextValue()  + offBase);
        const double w  = juce::jlimit(0.0, 1.0, smoothedFilterWidth.getNextValue() + offWidth);
        const double hq = juce::jlimit(0.0, 1.0, smoothedFilterHpQ.getNextValue()   + offHpQ);
        const double lq = juce::jlimit(0.0, 1.0, smoothedFilterLpQ.getNextValue()   + offLpQ);

        modulatedFilter[0].setParams(b, w, hq, lq, currentSampleRate);
        if (numCh > 1)
            modulatedFilter[1].setParams(b, w, hq, lq, currentSampleRate);

        for (int ch = 0; ch < juce::jmin(numCh, 2); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            // Same safety backstop as the other LFO-modulated paths (see applyPlaybackGain()'s
            // comment) -- offline testing (Tests/SmokeTest.cpp) shows this filter staying well
            // within this bound under stress, but it costs nothing to keep the backstop anyway.
            data[i] = juce::jlimit(-2.0f, 2.0f, modulatedFilter[ch].processSample(data[i]));
        }
    }

    return true;
}

void R3WRKAudioProcessor::applyPlaybackFilter(juce::AudioBuffer<float>& buffer, int numCh, int startSample, int numSamples,
                                              bool freshPlayPass)
{
    const double baseKnob  = juce::jlimit(0.0, 1.0, document.filterBase.load(std::memory_order_relaxed));
    const double widthKnob = juce::jlimit(0.0, 1.0, document.filterWidth.load(std::memory_order_relaxed));
    const double hpQKnob   = juce::jlimit(0.0, 1.0, document.filterHpQ.load(std::memory_order_relaxed));
    const double lpQKnob   = juce::jlimit(0.0, 1.0, document.filterLpQ.load(std::memory_order_relaxed));
    const auto   fm        = (r3wrk::FilterModel) juce::jlimit(0, 1, document.filterModel.load(std::memory_order_relaxed));

    if (freshPlayPass)
    {
        playbackFilter[0].reset();
        playbackFilter[1].reset();
        smoothedFilterBase.setCurrentAndTargetValue(baseKnob);
        smoothedFilterWidth.setCurrentAndTargetValue(widthKnob);
        smoothedFilterHpQ.setCurrentAndTargetValue(hpQKnob);
        smoothedFilterLpQ.setCurrentAndTargetValue(lpQKnob);
    }

    // Only the knob's own (slow, human-driven) value goes through the de-zippering smoother.
    // This function is now ONLY ever called for that manual, non-modulated case -- see
    // processBlock()'s comment: an actively LFO-modulated filter runs entirely through
    // applyModulatedFilter() instead, a separate TPT state-variable filter built to tolerate
    // continuous fast coefficient changes, which this direct-form biquad structurally isn't
    // (proven non-finite offline under that stress -- see Tests/SmokeTest.cpp).
    smoothedFilterBase.setTargetValue(baseKnob);
    smoothedFilterWidth.setTargetValue(widthKnob);
    smoothedFilterHpQ.setTargetValue(hpQKnob);
    smoothedFilterLpQ.setTargetValue(lpQKnob);
    const double b  = smoothedFilterBase.skip(numSamples);
    const double w  = smoothedFilterWidth.skip(numSamples);
    const double hq = smoothedFilterHpQ.skip(numSamples);
    const double lq = smoothedFilterLpQ.skip(numSamples);

    const bool engaged = r3wrk::filterEngaged(fm, b, w, hq, lq);

    // Re-engaging after a stretch bypassed leaves stale filter memory (z1/z2) computed under old,
    // possibly very different coefficients; feeding that straight into freshly recomputed
    // coefficients -- especially with resonance (Q) up -- can excite a loud transient "ping".
    // Clear it on every disengaged->engaged transition (cheap: a manual knob move is rare, not
    // per-cycle the way LFO modulation would be -- which is exactly why that case runs through
    // applyModulatedFilter() instead of here).
    if (engaged && ! lastFilterEngaged)
    {
        playbackFilter[0].reset();
        playbackFilter[1].reset();
    }
    lastFilterEngaged = engaged;

    if (! engaged)
        return;

    for (int ch = 0; ch < juce::jmin(numCh, 2); ++ch)
    {
        playbackFilter[ch].model = fm;
        playbackFilter[ch].setParams(b, w, hq, lq, currentSampleRate);
        playbackFilter[ch].processBlock(buffer.getWritePointer(ch) + startSample, numSamples);

        // Safety backstop: this LFO-modulated filter path has produced genuinely dangerous,
        // very loud output twice already while this feature was being built (a stale-state
        // transient, and a too-frequent coefficient recompute both did it before either was
        // diagnosed). A time-varying resonant biquad can in principle still overshoot in ways
        // not yet caught -- hard-clamp its output as a last line of defence so a future surprise
        // is a clipped/ugly sound, never an ear- or speaker-endangering spike.
        auto* out = buffer.getWritePointer(ch) + startSample;
        for (int i = 0; i < numSamples; ++i)
            out[i] = juce::jlimit(-2.0f, 2.0f, out[i]);
    }
}

void R3WRKAudioProcessor::applyPlaybackGain(juce::AudioBuffer<float>& buffer, int numCh, int startSample, int numSamples,
                                            const LfoModResult& lfoMod)
{
    const double gainKnobDb = document.playbackGainDb.load(std::memory_order_relaxed);
    const float knobTarget = std::abs(gainKnobDb) > 1.0e-3
        ? juce::Decibels::decibelsToGain((float) juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, gainKnobDb),
                                         (float) AudioDocument::kMinGainDb)
        : 1.0f;

    // Only the knob's own (slow, human-driven) value goes through the de-zippering smoother --
    // an LFO's contribution is applied AFTER smoothing, completely unsmoothed. Same bug,
    // independently found here, as applyPlaybackFilter() had: routing the LFO's offset THROUGH
    // this ~20ms ramp meant it couldn't keep re-targeting fast enough past a few Hz, so the
    // audible depth collapsed well below the "audio" rate band's advertised ceiling (heard as
    // "seems to work until around 28-30 Hz").
    smoothedGain.setTargetValue(knobTarget);
    const float g0knob = smoothedGain.getCurrentValue();
    const float g1knob = smoothedGain.skip(numSamples);

    // Held flat across this whole (kLfoModUpdateSamples-sized) chunk rather than also ramped --
    // matches the filter path's update granularity; fine at Gain's currently-usable rates.
    const float lfoGainLinear = lfoMod.gainDb != 0.0
        ? juce::Decibels::decibelsToGain((float) juce::jlimit(-60.0, 60.0, lfoMod.gainDb))
        : 1.0f;
    const float g0 = g0knob * lfoGainLinear;
    const float g1 = g1knob * lfoGainLinear;
    if (g0 == 1.0f && g1 == 1.0f)
        return;

    for (int ch = 0; ch < numCh; ++ch)
    {
        buffer.applyGainRamp(ch, startSample, numSamples, g0, g1);

        // Safety backstop, same reasoning as applyPlaybackFilter()'s: several LFOs could in
        // principle all target Gain at once and stack enough dB to be genuinely loud. Hard-clamp
        // so that's a clipped/ugly sound at worst, never an ear- or speaker-endangering spike.
        auto* out = buffer.getWritePointer(ch) + startSample;
        for (int i = 0; i < numSamples; ++i)
            out[i] = juce::jlimit(-2.0f, 2.0f, out[i]);
    }
}

float R3WRKAudioProcessor::applyReverb(juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                       bool freshPlayPass, bool tailOnly)
{
    const double size01     = juce::jlimit(0.0, 1.0, document.reverbSize.load(std::memory_order_relaxed));
    const double absorb01   = juce::jlimit(0.0, 1.0, document.reverbAbsorb.load(std::memory_order_relaxed));
    const double decay01    = juce::jlimit(0.0, 1.0, document.reverbDecay.load(std::memory_order_relaxed));
    const double tilt01     = juce::jlimit(0.0, 1.0, document.reverbTilt.load(std::memory_order_relaxed));
    const double mix01      = juce::jlimit(0.0, 1.0, document.reverbMix.load(std::memory_order_relaxed));
    const double predelay01 = juce::jlimit(0.0, 1.0, document.reverbPredelay.load(std::memory_order_relaxed));
    const double width01    = juce::jlimit(0.0, 1.0, document.reverbWidth.load(std::memory_order_relaxed));

    if (freshPlayPass)
    {
        // Deliberately does NOT reset reverbDsp here, unlike the filter's equivalent fresh-
        // play-pass handling (applyPlaybackFilter): a filter's z1/z2 state has no musical value
        // worth keeping across a stop (and old state meeting new coefficients can even produce
        // a transient), so clearing it is pure upside. A reverb's delay-line content is the
        // OPPOSITE -- it's a still-decaying tail, and wiping it here would silence it dead any
        // time wasPlaying flips false->true, including transiently (e.g. Scrub Mode's mousedown
        // sets isPlaying false for the scrub's duration) even though the user never asked the
        // reverb to stop. The only place reverbDsp is (correctly) reset is the enable/disable
        // edge below (lastReverbEngaged) -- an explicit user action, not an incidental one.
        smoothedReverbSize.setCurrentAndTargetValue(size01);
        smoothedReverbAbsorb.setCurrentAndTargetValue(absorb01);
        smoothedReverbDecay.setCurrentAndTargetValue(decay01);
        smoothedReverbTilt.setCurrentAndTargetValue(tilt01);
        smoothedReverbMix.setCurrentAndTargetValue(mix01);
        smoothedReverbPredelay.setCurrentAndTargetValue(predelay01);
        smoothedReverbWidth.setCurrentAndTargetValue(width01);
    }

    smoothedReverbSize.setTargetValue(size01);
    smoothedReverbAbsorb.setTargetValue(absorb01);
    smoothedReverbDecay.setTargetValue(decay01);
    smoothedReverbTilt.setTargetValue(tilt01);
    smoothedReverbMix.setTargetValue(mix01);
    smoothedReverbPredelay.setTargetValue(predelay01);
    smoothedReverbWidth.setTargetValue(width01);
    const double size     = smoothedReverbSize.skip(numSamples);
    const double absorb   = smoothedReverbAbsorb.skip(numSamples);
    const double decay    = smoothedReverbDecay.skip(numSamples);
    const double tilt     = smoothedReverbTilt.skip(numSamples);
    const double mix      = smoothedReverbMix.skip(numSamples);
    const double predelay = smoothedReverbPredelay.skip(numSamples);
    const double width    = smoothedReverbWidth.skip(numSamples);

    const bool engaged = document.reverbEnabled.load(std::memory_order_relaxed) && mix > 0.001;

    // Same reasoning as applyPlaybackFilter()'s: stale delay-line content computed under very
    // different parameters, suddenly fed fresh coefficients, can behave surprisingly. A manual
    // knob/enable move is rare (not per-cycle), so clearing on every disengaged->engaged edge is
    // cheap and the safer default -- unlike a filter, this does mean re-engaging drops whatever
    // tail was ringing from a previous engaged pass; acceptable for phase 1.
    if (engaged && ! lastReverbEngaged)
        reverbDsp.reset();
    lastReverbEngaged = engaged;

    if (! engaged)
        return 0.0f;

    const double predelayMs = juce::jmap(predelay, 7.0, 500.0);
    reverbDsp.setParams(size, absorb, decay, tilt, mix, predelayMs, width);

    float peak = 0.0f;
    if (numCh <= 1)
    {
        auto* data = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = tailOnly ? 0.0f : data[i];
            float outL, outR;
            reverbDsp.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            peak = juce::jmax(peak, std::abs(mono));
            data[i] = tailOnly ? (data[i] + mono) : mono;
        }
    }
    else
    {
        auto* left  = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = tailOnly ? 0.0f : left[i];
            const float inR = tailOnly ? 0.0f : right[i];
            float outL, outR;
            reverbDsp.processSample(inL, inR, outL, outR);
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
            left[i]  = tailOnly ? (left[i]  + outL) : outL;
            right[i] = tailOnly ? (right[i] + outR) : outR;
        }
    }
    return peak;
}

float R3WRKAudioProcessor::applyPlexiphon(juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                          bool freshPlayPass, bool tailOnly)
{
    const double level01   = juce::jlimit(0.0, 1.0, document.plexLevel.load(std::memory_order_relaxed));
    const double plexus01  = juce::jlimit(0.0, 1.0, document.plexPlexus.load(std::memory_order_relaxed));
    const double size01    = juce::jlimit(0.0, 1.0, document.plexSize.load(std::memory_order_relaxed));
    const double diffuse01 = juce::jlimit(0.0, 1.0, document.plexDiffuse.load(std::memory_order_relaxed));
    const double decay01   = juce::jlimit(0.0, 1.0, document.plexDecay.load(std::memory_order_relaxed));
    const double color01   = juce::jlimit(0.0, 1.0, document.plexColor.load(std::memory_order_relaxed));
    const double mix01     = juce::jlimit(0.0, 1.0, document.plexMix.load(std::memory_order_relaxed));

    if (freshPlayPass)
    {
        // Deliberately does NOT reset plexDsp here -- same reasoning as applyReverb()'s
        // comment: a long-sustaining tail (this module's whole "super-infinite" identity) has
        // no business being wiped by an incidental wasPlaying flip (Scrub Mode, etc.) that
        // isn't a deliberate stop. Only the enable/disable edge below resets it.
        smoothedPlexLevel.setCurrentAndTargetValue(level01);
        smoothedPlexPlexus.setCurrentAndTargetValue(plexus01);
        smoothedPlexSize.setCurrentAndTargetValue(size01);
        smoothedPlexDiffuse.setCurrentAndTargetValue(diffuse01);
        smoothedPlexDecay.setCurrentAndTargetValue(decay01);
        smoothedPlexColor.setCurrentAndTargetValue(color01);
        smoothedPlexMix.setCurrentAndTargetValue(mix01);
    }

    smoothedPlexLevel.setTargetValue(level01);
    smoothedPlexPlexus.setTargetValue(plexus01);
    smoothedPlexSize.setTargetValue(size01);
    smoothedPlexDiffuse.setTargetValue(diffuse01);
    smoothedPlexDecay.setTargetValue(decay01);
    smoothedPlexColor.setTargetValue(color01);
    smoothedPlexMix.setTargetValue(mix01);
    const double level   = smoothedPlexLevel.skip(numSamples);
    const double plexus  = smoothedPlexPlexus.skip(numSamples);
    const double size    = smoothedPlexSize.skip(numSamples);
    const double diffuse = smoothedPlexDiffuse.skip(numSamples);
    const double decay   = smoothedPlexDecay.skip(numSamples);
    const double color   = smoothedPlexColor.skip(numSamples);
    const double mix     = smoothedPlexMix.skip(numSamples);

    const bool engaged = document.plexEnabled.load(std::memory_order_relaxed) && mix > 0.001;

    if (engaged && ! lastPlexEngaged)
        plexDsp.reset();
    lastPlexEngaged = engaged;

    if (! engaged)
        return 0.0f;

    plexDsp.setParams(level, plexus, size, diffuse, decay, color, mix, numSamples);

    float peak = 0.0f;
    if (numCh <= 1)
    {
        auto* data = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = tailOnly ? 0.0f : data[i];
            float outL, outR;
            plexDsp.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            peak = juce::jmax(peak, std::abs(mono));
            data[i] = tailOnly ? (data[i] + mono) : mono;
        }
    }
    else
    {
        auto* left  = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = tailOnly ? 0.0f : left[i];
            const float inR = tailOnly ? 0.0f : right[i];
            float outL, outR;
            plexDsp.processSample(inL, inR, outL, outR);
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
            left[i]  = tailOnly ? (left[i]  + outL) : outL;
            right[i] = tailOnly ? (right[i] + outR) : outR;
        }
    }
    return peak;
}

float R3WRKAudioProcessor::applyMimeophon(juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                          bool freshPlayPass, bool tailOnly)
{
    const double zone01    = juce::jlimit(0.0, 1.0, document.mimeoZone.load(std::memory_order_relaxed));
    const double rate01    = juce::jlimit(0.0, 1.0, document.mimeoRate.load(std::memory_order_relaxed));
    const double repeats01 = juce::jlimit(0.0, 1.0, document.mimeoRepeats.load(std::memory_order_relaxed));
    const double color01   = juce::jlimit(0.0, 1.0, document.mimeoColor.load(std::memory_order_relaxed));
    const double halo01    = juce::jlimit(0.0, 1.0, document.mimeoHalo.load(std::memory_order_relaxed));
    const double mix01     = juce::jlimit(0.0, 1.0, document.mimeoMix.load(std::memory_order_relaxed));
    const double skew01    = juce::jlimit(0.0, 1.0, document.mimeoSkew.load(std::memory_order_relaxed));
    const bool pingPongOn  = document.mimeoPingPong.load(std::memory_order_relaxed);

    if (freshPlayPass)
    {
        // Deliberately does NOT reset mimeoDsp here -- same reasoning as applyReverb()'s/
        // applyPlexiphon()'s comment: Repeats can be pushed to self-oscillation, and that tail
        // has no business being wiped by an incidental wasPlaying flip (Scrub Mode, etc.) that
        // isn't a deliberate stop. Only the enable/disable edge below resets it.
        smoothedMimeoZone.setCurrentAndTargetValue(zone01);
        smoothedMimeoRate.setCurrentAndTargetValue(rate01);
        smoothedMimeoRepeats.setCurrentAndTargetValue(repeats01);
        smoothedMimeoColor.setCurrentAndTargetValue(color01);
        smoothedMimeoHalo.setCurrentAndTargetValue(halo01);
        smoothedMimeoMix.setCurrentAndTargetValue(mix01);
        smoothedMimeoSkew.setCurrentAndTargetValue(skew01);
    }

    smoothedMimeoZone.setTargetValue(zone01);
    smoothedMimeoRate.setTargetValue(rate01);
    smoothedMimeoRepeats.setTargetValue(repeats01);
    smoothedMimeoColor.setTargetValue(color01);
    smoothedMimeoHalo.setTargetValue(halo01);
    smoothedMimeoMix.setTargetValue(mix01);
    smoothedMimeoSkew.setTargetValue(skew01);
    const double zone    = smoothedMimeoZone.skip(numSamples);
    const double rate    = smoothedMimeoRate.skip(numSamples);
    const double repeats = smoothedMimeoRepeats.skip(numSamples);
    const double color   = smoothedMimeoColor.skip(numSamples);
    const double halo    = smoothedMimeoHalo.skip(numSamples);
    const double skew    = smoothedMimeoSkew.skip(numSamples);
    const double mix     = smoothedMimeoMix.skip(numSamples);

    const bool engaged = document.mimeoEnabled.load(std::memory_order_relaxed) && mix > 0.001;

    if (engaged && ! lastMimeoEngaged)
        mimeoDsp.reset();
    lastMimeoEngaged = engaged;

    if (! engaged)
        return 0.0f;

    mimeoDsp.setParams(zone, rate, repeats, color, halo, mix, skew, pingPongOn);

    float peak = 0.0f;
    if (numCh <= 1)
    {
        auto* data = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = tailOnly ? 0.0f : data[i];
            float outL, outR;
            mimeoDsp.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            peak = juce::jmax(peak, std::abs(mono));
            data[i] = tailOnly ? (data[i] + mono) : mono;
        }
    }
    else
    {
        auto* left  = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = tailOnly ? 0.0f : left[i];
            const float inR = tailOnly ? 0.0f : right[i];
            float outL, outR;
            mimeoDsp.processSample(inL, inR, outL, outR);
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
            left[i]  = tailOnly ? (left[i]  + outL) : outL;
            right[i] = tailOnly ? (right[i] + outR) : outR;
        }
    }
    return peak;
}

void R3WRKAudioProcessor::captureOutput(const juce::AudioBuffer<float>& out, int numCh, int numSamples)
{
    if (! capturingOutput.load(std::memory_order_relaxed) || numSamples <= 0 || numCh <= 0)
        return;

    const int ch = juce::jlimit(1, 2, numCh);
    const int64_t needed = outputCaptureWritePos + numSamples;
    if (outputCaptureBuffer.getNumChannels() < ch || (int64_t) outputCaptureBuffer.getNumSamples() < needed)
    {
        int64_t cap = juce::jmax((int64_t) outputCaptureBuffer.getNumSamples(),
                                 (int64_t) (currentSampleRate * 30.0));
        while (cap < needed) cap *= 2;
        outputCaptureBuffer.setSize(ch, (int) cap, true, true, true);
    }
    for (int c = 0; c < ch; ++c)
        outputCaptureBuffer.copyFrom(c, (int) outputCaptureWritePos, out, juce::jmin(c, numCh - 1), 0, numSamples);
    outputCaptureWritePos += numSamples;
}

// Black Box: VST/AU only (see the header comment on isBlackBoxAvailable()). Reallocating
// always drops whatever the ring currently holds -- same trade-off a sample-rate change
// already made before duration became adjustable, and losing the last take's-worth of
// history on a rare mid-session change (or a duration switch) is an acceptable cost for not
// carrying resampling/splicing machinery just for this.
void R3WRKAudioProcessor::reallocateBlackBoxBuffer()
{
    if (wrapperType == wrapperType_Standalone)
        return;

    const int chans = juce::jmax(1, getTotalNumInputChannels());
    const int capacity = (int) (blackBoxDurationSecs * currentSampleRate);
    const juce::ScopedLock sl(blackBoxLock);
    blackBoxBuffer.setSize(chans, capacity, false, true, true);
    blackBoxBuffer.clear();
    blackBoxCapacity = capacity;
    blackBoxWritePos.store(0, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::setBlackBoxDurationSecs(double secs)
{
    blackBoxDurationSecs = secs < 195.0 ? kBlackBoxDurationShort : kBlackBoxDurationLong;   // snap to a valid choice
    reallocateBlackBoxBuffer();
}

void R3WRKAudioProcessor::appendToBlackBox(const juce::AudioBuffer<float>& buffer, int numCh, int numSamples)
{
    if (numSamples <= 0 || numCh <= 0)
        return;
    if (! blackBoxLock.tryEnter())   // the UI is mid-snapshot -- drop this block, never block here
        return;

    const int chans = juce::jmin(numCh, blackBoxBuffer.getNumChannels());
    const int64_t pos = blackBoxWritePos.load(std::memory_order_relaxed);

    for (int ch = 0; ch < chans; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        int done = 0;
        while (done < numSamples)
        {
            const int writeIdx = (int) ((pos + done) % blackBoxCapacity);
            const int chunk = juce::jmin(numSamples - done, blackBoxCapacity - writeIdx);
            blackBoxBuffer.copyFrom(ch, writeIdx, src + done, chunk);
            done += chunk;
        }
    }
    blackBoxWritePos.store(pos + numSamples, std::memory_order_relaxed);
    blackBoxLock.exit();
}

juce::AudioBuffer<float> R3WRKAudioProcessor::getBlackBoxSnapshot(double& sampleRateOut) const
{
    const juce::ScopedLock sl(blackBoxLock);
    sampleRateOut = currentSampleRate;

    if (blackBoxCapacity <= 0)
        return {};

    const int64_t pos = blackBoxWritePos.load(std::memory_order_relaxed);
    const int filled = (int) juce::jmin<int64_t>(pos, blackBoxCapacity);
    juce::AudioBuffer<float> out(blackBoxBuffer.getNumChannels(), filled);
    if (filled == 0)
        return out;

    if (pos <= blackBoxCapacity)
    {
        // Hasn't wrapped yet -- everything written so far is already in chronological order
        // starting at 0.
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.copyFrom(ch, 0, blackBoxBuffer, ch, 0, filled);
    }
    else
    {
        // Wrapped at least once: the oldest surviving sample sits right where the next write
        // will land. Unwrap into chronological order: [startIdx..end) then [0..startIdx).
        const int startIdx = (int) (pos % blackBoxCapacity);
        const int tail = blackBoxCapacity - startIdx;
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            out.copyFrom(ch, 0,    blackBoxBuffer, ch, startIdx, tail);
            out.copyFrom(ch, tail, blackBoxBuffer, ch, 0,        startIdx);
        }
    }
    return out;
}

void R3WRKAudioProcessor::startBlackBoxPreview(juce::AudioBuffer<float> audio, double sourceRate)
{
    const juce::ScopedLock sl(blackBoxLock);
    blackBoxPreviewPlaying.store(false, std::memory_order_relaxed);   // stop any current preview first
    blackBoxPreviewBuffer = (sourceRate > 0.0 && std::abs(sourceRate - currentSampleRate) > 0.5)
                           ? AudioDocument::resampled(audio, sourceRate, currentSampleRate)
                           : std::move(audio);
    blackBoxPreviewPos.store(0, std::memory_order_relaxed);
    blackBoxPreviewPlaying.store(blackBoxPreviewBuffer.getNumSamples() > 0, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::stopBlackBoxPreview()
{
    blackBoxPreviewPlaying.store(false, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::renderBlackBoxPreview(juce::AudioBuffer<float>& out, int numCh, int numSamples)
{
    if (! blackBoxLock.tryEnter())   // the message thread is mid-(re)start -- silence this block rather than block
    {
        out.clear();
        return;
    }

    const int totalCh = blackBoxPreviewBuffer.getNumChannels();
    const int64_t len = blackBoxPreviewBuffer.getNumSamples();
    int64_t pos = blackBoxPreviewPos.load(std::memory_order_relaxed);

    if (len <= 0 || totalCh <= 0)
    {
        blackBoxPreviewPlaying.store(false, std::memory_order_relaxed);
        out.clear();
        blackBoxLock.exit();
        return;
    }

    // Loops for as long as the popup's selection stays put -- see startBlackBoxPreview()'s
    // header comment. No crossfade at the wrap; a hard loop is the point (quick audition of
    // whatever's selected), not a polished loop point the way the main document's loop
    // crossfade is for actual playback.
    for (int i = 0; i < numSamples; ++i)
    {
        if (pos >= len)
            pos = 0;
        for (int ch = 0; ch < numCh; ++ch)
            out.setSample(ch, i, blackBoxPreviewBuffer.getSample(juce::jmin(ch, totalCh - 1), (int) pos));
        ++pos;
    }
    blackBoxPreviewPos.store(pos, std::memory_order_relaxed);
    blackBoxLock.exit();
}

void R3WRKAudioProcessor::startOutputCapture()
{
    if (capturingOutput.load(std::memory_order_relaxed))
        return;
    outputCaptureWritePos = 0;
    outputCaptureBuffer.setSize(2, (int) juce::jmax(1.0, currentSampleRate * 30.0), false, true, true);
    capturingOutput.store(true, std::memory_order_relaxed);   // last: audio thread only appends once the buffer's ready
}

juce::File R3WRKAudioProcessor::stopOutputCaptureAndWrite(const juce::File& dest, const AudioSaveOptions& opts)
{
    capturingOutput.store(false, std::memory_order_relaxed);

    const int64_t n = outputCaptureWritePos;
    const int ch = outputCaptureBuffer.getNumChannels();
    if (n <= 0 || ch <= 0)
    {
        outputCaptureBuffer.setSize(0, 0);
        outputCaptureWritePos = 0;
        return {};
    }

    juce::AudioBuffer<float> finalBuf(ch, (int) n);
    for (int c = 0; c < ch; ++c)
        finalBuf.copyFrom(c, 0, outputCaptureBuffer, c, 0, (int) n);
    outputCaptureBuffer.setSize(0, 0);
    outputCaptureWritePos = 0;

    const juce::File out = dest.withFileExtension(opts.extension());
    if (! AudioDocument::writeAudioFile(std::move(finalBuf), currentSampleRate, out, opts))
        return {};
    return out;
}

void R3WRKAudioProcessor::startRecording()
{
    recordingWritePos = 0;
    document.resetRecordingScope();
    int chans = getTotalNumInputChannels() > 0 ? getTotalNumInputChannels() : 2;
    recordingAccumulator.setSize(chans, (int) juce::jmax(1.0, currentSampleRate * 4.0), false, true, true);
    document.isPlaying = false;
    document.isRecording = true;
}

void R3WRKAudioProcessor::stopRecording()
{
    document.isRecording = false;

    if (recordingWritePos <= 0)   // stopped before any audio was captured -- leave the document alone
        return;

    juce::AudioBuffer<float> finalBuffer(recordingAccumulator.getNumChannels(), (int) recordingWritePos);
    for (int ch = 0; ch < finalBuffer.getNumChannels(); ++ch)
        finalBuffer.copyFrom(ch, 0, recordingAccumulator, ch, 0, (int) recordingWritePos);

    document.beginChange();
    document.setSampleRate(currentSampleRate);
    document.setSourceBitDepth(32, true);   // captured as 32-bit float
    document.commitChange(std::move(finalBuffer), "Record");
    document.loopStart = 0;
    document.loopEnd = document.getNumSamples();
    document.markAsOriginal();   // this take is the new "Revert to Original" baseline
}

//==============================================================================
void R3WRKAudioProcessor::startDesktopRecording()
{
    if (! DesktopAudioCapture::isSupported())
    {
        if (onDesktopStatus) onDesktopStatus("Desktop capture needs macOS 13 or later");
        return;
    }

    {
        const juce::ScopedLock sl(desktopRecLock);
        desktopRecBuffer.setSize(2, 0);
        desktopRecWritePos = 0;
        desktopRecChannels = 2;
        desktopRecRate = 48000.0;
    }
    document.resetRecordingScope();
    document.isPlaying = false;

    desktopCapture.start(
        [this](const float* const* d, int nc, int nf, double sr) { appendDesktopSamples(d, nc, nf, sr); },
        [this]
        {
            juce::MessageManager::callAsync([this]
            {
                desktopRecording = true;
                document.isRecording = true;
                document.notifyChanged();
            });
        },
        [this](juce::String msg)
        {
            juce::MessageManager::callAsync([this, msg]
            {
                const bool wasRunning = desktopRecording.exchange(false);
                document.isRecording = false;
                if (onDesktopStatus)
                    onDesktopStatus(msg.isNotEmpty() ? msg
                                                    : juce::String("Couldn't start desktop capture"));
                if (wasRunning)
                    finalizeDesktopRecording();   // stream died mid-take -- keep what landed
                document.notifyChanged();
            });
        });
}

void R3WRKAudioProcessor::stopDesktopRecording()
{
    desktopCapture.stop();          // sets running=false, stops the stream, drains the queue
    desktopRecording = false;
    document.isRecording = false;
    finalizeDesktopRecording();
}

void R3WRKAudioProcessor::appendDesktopSamples(const float* const* data, int numChannels, int numFrames, double sr)
{
    if (numFrames <= 0 || numChannels <= 0 || data == nullptr)
        return;

    const juce::ScopedLock sl(desktopRecLock);
    const int ch = juce::jlimit(1, 2, numChannels);
    desktopRecChannels = ch;
    desktopRecRate = sr > 0.0 ? sr : 48000.0;

    const int64_t needed = desktopRecWritePos + numFrames;
    if ((int64_t) desktopRecBuffer.getNumSamples() < needed || desktopRecBuffer.getNumChannels() < ch)
    {
        int64_t cap = juce::jmax((int64_t) desktopRecBuffer.getNumSamples(),
                                 (int64_t) (desktopRecRate * 8.0));
        while (cap < needed) cap *= 2;
        desktopRecBuffer.setSize(ch, (int) cap, true, true, true);
    }
    for (int c = 0; c < ch; ++c)
        if (data[c] != nullptr)
            desktopRecBuffer.copyFrom(c, (int) desktopRecWritePos, data[c], numFrames);
    desktopRecWritePos += numFrames;

    // Feed the live scope, same shape as the input-record path.
    constexpr int hop = 256;
    for (int s = 0; s < numFrames; s += hop)
    {
        const int nn = juce::jmin(hop, numFrames - s);
        float mn = 0.0f, mx = 0.0f;
        for (int c = 0; c < ch; ++c)
        {
            if (data[c] == nullptr) continue;
            const auto r = juce::FloatVectorOperations::findMinAndMax(data[c] + s, nn);
            mn = juce::jmin(mn, r.getStart());
            mx = juce::jmax(mx, r.getEnd());
        }
        const int wpos = document.scopeWritePos.load(std::memory_order_relaxed);
        document.scopeMin[wpos] = mn;
        document.scopeMax[wpos] = mx;
        document.scopeWritePos.store((wpos + 1) % AudioDocument::scopeSize, std::memory_order_release);
    }
    document.recordedSamples.store(desktopRecWritePos, std::memory_order_relaxed);
}

void R3WRKAudioProcessor::finalizeDesktopRecording()
{
    juce::AudioBuffer<float> finalBuf;
    double rate = 48000.0;
    {
        const juce::ScopedLock sl(desktopRecLock);
        if (desktopRecWritePos <= 0)
        {
            if (onDesktopStatus) onDesktopStatus("No desktop audio captured");
            return;
        }
        finalBuf.setSize(desktopRecChannels, (int) desktopRecWritePos);
        for (int c = 0; c < desktopRecChannels; ++c)
            finalBuf.copyFrom(c, 0, desktopRecBuffer, c, 0, (int) desktopRecWritePos);
        rate = desktopRecRate;
        desktopRecBuffer.setSize(0, 0);
        desktopRecWritePos = 0;
    }

    document.beginChange();
    document.setSampleRate(rate);
    document.setSourceBitDepth(32, true);   // captured as 32-bit float
    document.commitChange(std::move(finalBuf), "Record Desktop");
    document.loopStart = 0;
    document.loopEnd = document.getNumSamples();
    document.markAsOriginal();
    if (onDesktopStatus) onDesktopStatus("Desktop recording captured");
}

void R3WRKAudioProcessor::startPlayback()
{
    if (document.isEmpty())
        return;
    document.isRecording = false;

    const int64_t n = document.getNumSamples();
    int64_t p = document.playhead.load();
    const auto sel = document.getSelection();
    if (sel.getEnd() > sel.getStart())
    {
        const int64_t s = sel.getStart(), e = sel.getEnd();
        if (p < s || p >= e)          // start from the selection unless the cursor is inside it
            p = s;
    }
    else if (p >= n)
    {
        p = 0;
    }
    document.playhead = p;
    document.isPlaying = true;
}

void R3WRKAudioProcessor::stopPlayback()
{
    document.isPlaying = false;
}

juce::AudioProcessorEditor* R3WRKAudioProcessor::createEditor()
{
    return new R3WRKAudioProcessorEditor(*this);
}

void R3WRKAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out(destData, false);
    out.writeInt(kStateMagic);
    out.writeDouble(document.getSampleRate());
    out.writeInt(document.getNumChannels());
    out.writeInt64(document.getNumSamples());
    out.writeInt64(document.loopStart.load());
    out.writeInt64(document.loopEnd.load());
    out.writeBool(document.loopEnabled.load());
    out.writeBool(document.loopPingPong.load());
    out.writeBool(document.loopReverse.load());
    out.writeInt64(document.getSelectionStart());
    out.writeInt64(document.getSelectionEnd());
    out.writeDouble(document.playbackSpeed.load());
    out.writeDouble(document.playbackPitch.load());
    out.writeDouble(document.playbackStretch.load());
    out.writeDouble(document.autoRecordThresholdDb.load());
    out.writeDouble(document.filterBase.load());
    out.writeDouble(document.filterWidth.load());
    out.writeDouble(document.filterHpQ.load());
    out.writeDouble(document.filterLpQ.load());
    out.writeDouble(document.playbackGainDb.load());
    out.writeInt(document.filterModel.load());
    out.writeDouble(document.loopCrossfadeMs.load());
    out.writeBool(document.bakeLoopCrossfadeOnExport.load());
    out.writeString(document.getSourceFilePath());   // R3WG+ -- see EditorToolbar::setCurrentFile()

    const int numLfos = juce::jlimit(0, AudioDocument::kMaxLfos, document.numVisibleLfos.load());   // R3WI+
    out.writeInt(numLfos);
    for (int i = 0; i < numLfos; ++i)
    {
        auto& s = document.lfoSlots[i];
        out.writeBool(s.enabled.load());
        out.writeInt(s.shape.load());
        out.writeInt(s.rateRange.load());
        out.writeDouble(s.rate01.load());
        out.writeInt(s.target.load());
        out.writeDouble(s.amount.load());
        out.writeBool(s.tempoSync.load());
    }

    out.writeBool(document.reverbEnabled.load());   // R3WJ+
    out.writeDouble(document.reverbSize.load());
    out.writeDouble(document.reverbAbsorb.load());
    out.writeDouble(document.reverbDecay.load());
    out.writeDouble(document.reverbTilt.load());
    out.writeDouble(document.reverbMix.load());
    out.writeDouble(document.reverbPredelay.load());
    out.writeDouble(document.reverbWidth.load());   // R3WK+

    out.writeBool(document.plexEnabled.load());   // R3WL+
    out.writeDouble(document.plexLevel.load());
    out.writeDouble(document.plexPlexus.load());
    out.writeDouble(document.plexSize.load());
    out.writeDouble(document.plexDiffuse.load());
    out.writeDouble(document.plexDecay.load());
    out.writeDouble(document.plexColor.load());
    out.writeDouble(document.plexMix.load());

    out.writeBool(document.mimeoEnabled.load());   // R3WM+
    out.writeDouble(document.mimeoZone.load());
    out.writeDouble(document.mimeoRate.load());
    out.writeDouble(document.mimeoRepeats.load());
    out.writeDouble(document.mimeoColor.load());
    out.writeDouble(document.mimeoHalo.load());
    out.writeDouble(document.mimeoMix.load());
    out.writeDouble(document.mimeoSkew.load());   // R3WN+
    out.writeBool(document.mimeoPingPong.load());   // R3WO+

    auto& buf = document.getBuffer();
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        out.write(buf.getReadPointer(ch), (size_t) buf.getNumSamples() * sizeof(float));

    // Slice markers are deliberately NOT persisted -- they live only in AudioDocument for the
    // life of this plugin instance (per the user: "save for that session, not in permanent
    // memory"). Older state blobs may still carry a trailing marker list here; setStateInformation
    // just stops reading after the audio, so those extra bytes are harmless.
}

void R3WRKAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in(data, (size_t) sizeInBytes, false);
    const int magic = in.readInt();
    if (magic != kStateMagic && magic != kStateMagicR3WN && magic != kStateMagicR3WM && magic != kStateMagicR3WL && magic != kStateMagicR3WK && magic != kStateMagicR3WJ && magic != kStateMagicR3WI && magic != kStateMagicR3WH && magic != kStateMagicR3WG && magic != kStateMagicR3WF && magic != kStateMagicR3WE && magic != kStateMagicR3WD
        && magic != kStateMagicR3WC && magic != kStateMagicR3WB && magic != kStateMagicR3WA
        && magic != kStateMagicR3W9 && magic != kStateMagicR3W8 && magic != kStateMagicR3W7
        && magic != kStateMagicR3W6 && magic != kStateMagicR3W5)
        return;
    // R3WO: adds Mimeophon Ping-Pong after Skew. R3WN: adds Mimeophon Skew after the other
    // Mimeophon params. R3WM: adds the Mimeophon
    // params after the Plexiphon params. R3WL: adds the Plexiphon
    // params after reverb Width. R3WK: adds reverb Width after Pre-
    // delay. R3WJ: adds the reverb params after the LFO
    // modulation slots. R3WI: adds the LFO
    // modulation slots after the source file path. R3WH: adds the
    // loopReverse flag after loopPingPong. R3WG: adds the source file path
    // after bakeLoopCrossfadeOnExport. R3WF: adds
    // bakeLoopCrossfadeOnExport after loopCrossfadeMs. R3WE: adds loopCrossfadeMs
    // after filterModel. R3WD: adds filterModel (MnM / Octatrack) after playbackGainDb. R3WC:
    // adds the loopPingPong flag after loopEnabled. R3WB: filter is Base/Width/HP Q/LP Q (the
    // MnM model). R3W8..R3WA: the old OT-style Base/Width filter with a single resonance
    // (+ later Drive, + later 12/24 slope). R3W6/R3W7: the even older mode/cutoff filter.
    // R3W5: none.
    const bool hasMimeoPingPong    = (magic == kStateMagic);                                  // R3WO
    const bool hasMimeoSkew        = (hasMimeoPingPong || magic == kStateMagicR3WN);          // R3WN+
    const bool hasMimeo            = (hasMimeoSkew || magic == kStateMagicR3WM);              // R3WM+
    const bool hasPlex             = (hasMimeo || magic == kStateMagicR3WL);                  // R3WL+
    const bool hasReverbWidth      = (hasPlex || magic == kStateMagicR3WK);                   // R3WK+
    const bool hasReverb           = (hasReverbWidth || magic == kStateMagicR3WJ);            // R3WJ+
    const bool hasLfoSlots        = (hasReverb || magic == kStateMagicR3WI);                  // R3WI+
    const bool hasReverseLoop     = (hasLfoSlots || magic == kStateMagicR3WH);                // R3WH+
    const bool hasSourceFilePath  = (hasReverseLoop || magic == kStateMagicR3WG);             // R3WG+
    const bool hasLoopXfadeBake   = (hasSourceFilePath || magic == kStateMagicR3WF);          // R3WF+
    const bool hasLoopXfade       = (hasLoopXfadeBake || magic == kStateMagicR3WE);           // R3WE+
    const bool hasFilterModel     = (hasLoopXfade || magic == kStateMagicR3WD);               // R3WD+
    const bool hasPingPong        = (hasFilterModel || magic == kStateMagicR3WC);             // R3WC+
    const bool hasMnmFilter       = (hasPingPong || magic == kStateMagicR3WB);                // R3WB+
    const bool hasOldBaseWidth    = (magic == kStateMagicR3WA || magic == kStateMagicR3W9
                                     || magic == kStateMagicR3W8);                            // R3W8..R3WA
    const bool hasDriveField      = (magic == kStateMagicR3WA || magic == kStateMagicR3W9);   // R3W9/R3WA
    const bool hasSlopeField      = (magic == kStateMagicR3WA);                               // R3WA
    const bool hasOldModeFilter   = (magic == kStateMagicR3W7 || magic == kStateMagicR3W6);
    const bool hasGainField       = (hasMnmFilter || hasOldBaseWidth || magic == kStateMagicR3W7); // R3W7+

    double sr = in.readDouble();
    int numCh = in.readInt();
    int64_t numSamples = in.readInt64();
    int64_t lStart = in.readInt64();
    int64_t lEnd = in.readInt64();
    bool lEnabled = in.readBool();
    bool lPingPong = hasPingPong ? in.readBool() : false;
    bool lReverse = hasReverseLoop ? in.readBool() : false;
    int64_t selStartS = in.readInt64();
    int64_t selEndS = in.readInt64();
    double spd = in.readDouble();
    double pch = in.readDouble();
    double str = in.readDouble();
    double thresh = in.readDouble();

    double fBase = 0.0, fWidth = 1.0, fHpQ = 0.0, fLpQ = 0.0, gDb = 0.0;
    if (hasMnmFilter)
    {
        fBase  = in.readDouble();
        fWidth = in.readDouble();
        fHpQ   = in.readDouble();
        fLpQ   = in.readDouble();
    }
    else if (hasOldBaseWidth)
    {
        // The old OT-style Base/Width filter: keep Base/Width (they mean the same thing), and
        // fold its single resonance onto BOTH edges' Q. Drive and the 12/24 slope switch have
        // no equivalent in the MnM model -- read past them, drop them.
        fBase  = in.readDouble();
        fWidth = in.readDouble();
        const double oldRes = in.readDouble();
        fHpQ = fLpQ = juce::jlimit(0.0, 1.0, oldRes);
        if (hasDriveField) in.readDouble();          // old filterDrive -- dropped
        if (hasSlopeField) { in.readBool(); in.readBool(); }  // old 12/24 slope -- dropped
    }
    else if (hasOldModeFilter)
    {
        // Migrate the oldest mode/cutoff filter onto Base/Width so a returning session keeps
        // roughly the same sound. Notch (mode 4) has no equivalent -> open. Old resonance is
        // read but not carried (it doesn't map onto the two-edge Q sensibly).
        const int    oldMode = in.readInt();
        const double oldCut  = in.readDouble();
        in.readDouble();          // old filterResonance -- consumed, not carried over
        const double pos = r3wrk::filterHzToPos(oldCut > 0.0 ? oldCut : 1000.0);
        switch (oldMode)
        {
            case 1: fBase = 0.0;                              fWidth = pos;  break;  // low-pass
            case 2: fBase = pos;                              fWidth = 1.0;  break;  // high-pass
            case 3: fBase = juce::jlimit(0.0, 1.0, pos - 0.06); fWidth = 0.12; break;  // band-pass
            default: fBase = 0.0;                             fWidth = 1.0;  break;  // off / notch
        }
    }
    if (hasGainField)
        gDb = in.readDouble();
    const int fModel = hasFilterModel ? in.readInt() : 0;
    const double loopXfadeMs = hasLoopXfade ? in.readDouble() : 0.0;
    const bool loopXfadeBake = hasLoopXfadeBake ? in.readBool() : false;
    const juce::String sourceFilePath = hasSourceFilePath ? in.readString() : juce::String();

    struct LoadedLfo
    {
        bool enabled = false;
        int shape = (int) r3wrk::LfoShape::sine, rateRange = (int) r3wrk::LfoRateRange::normal, target = (int) r3wrk::ModTarget::none;
        double rate01 = 0.3, amount = 0.0;
        bool tempoSync = false;
    };
    LoadedLfo loadedLfos[AudioDocument::kMaxLfos];
    int loadedNumLfos = 1;   // an older state blob has no LFOs -- restore to one default (disabled) slot
    if (hasLfoSlots)
    {
        // The blob's own count can exceed today's kMaxLfos (e.g. a project saved by a build that
        // allowed more slots) -- read every one it says it wrote, so the stream stays byte-aligned
        // for whatever follows, and just don't keep the ones past kMaxLfos.
        const int rawNumLfos = in.readInt();
        for (int i = 0; i < rawNumLfos; ++i)
        {
            LoadedLfo s;
            s.enabled   = in.readBool();
            s.shape     = in.readInt();
            s.rateRange = in.readInt();
            s.rate01    = in.readDouble();
            s.target    = in.readInt();
            s.amount    = in.readDouble();
            s.tempoSync = in.readBool();
            if (i < AudioDocument::kMaxLfos)
                loadedLfos[i] = s;
        }
        loadedNumLfos = juce::jlimit(0, AudioDocument::kMaxLfos, rawNumLfos);
    }

    // An older state blob has no reverb -- restore the same non-destructive defaults
    // AudioDocument itself declares (enabled=false, mix=0), so loading one doesn't retroactively
    // turn a reverb on that was never there.
    bool   rvEnabled  = false;
    double rvSize     = 0.5, rvAbsorb = 0.5, rvDecay = 0.5, rvTilt = 0.5, rvMix = 0.0, rvPredelay = 0.1, rvWidth = 0.5;
    if (hasReverb)
    {
        rvEnabled  = in.readBool();
        rvSize     = in.readDouble();
        rvAbsorb   = in.readDouble();
        rvDecay    = in.readDouble();
        rvTilt     = in.readDouble();
        rvMix      = in.readDouble();
        rvPredelay = in.readDouble();
    }
    if (hasReverbWidth)
        rvWidth = in.readDouble();

    // An older state blob has no Plexiphon -- same non-destructive defaults AudioDocument
    // itself declares (enabled=false, mix=0).
    bool   pxEnabled = false;
    double pxLevel = 0.5, pxPlexus = 0.5, pxSize = 0.5, pxDiffuse = 0.5, pxDecay = 0.5,
           pxColor = 0.5, pxMix = 0.0;
    if (hasPlex)
    {
        pxEnabled = in.readBool();
        pxLevel   = in.readDouble();
        pxPlexus  = in.readDouble();
        pxSize    = in.readDouble();
        pxDiffuse = in.readDouble();
        pxDecay   = in.readDouble();
        pxColor   = in.readDouble();
        pxMix     = in.readDouble();
    }

    // An older state blob has no Mimeophon -- same non-destructive defaults AudioDocument
    // itself declares (enabled=false, mix=0).
    bool   mxEnabled = false;
    double mxZone = 3.0 / 7.0, mxRate = 0.5, mxRepeats = 0.3, mxColor = 0.5, mxHalo = 0.0, mxMix = 0.0, mxSkew = 0.5;
    bool mxPingPong = false;
    if (hasMimeo)
    {
        mxEnabled = in.readBool();
        mxZone    = in.readDouble();
        mxRate    = in.readDouble();
        mxRepeats = in.readDouble();
        mxColor   = in.readDouble();
        mxHalo    = in.readDouble();
        mxMix     = in.readDouble();
    }
    if (hasMimeoSkew)
        mxSkew = in.readDouble();
    if (hasMimeoPingPong)
        mxPingPong = in.readBool();

    if (numCh <= 0 || numCh > kMaxStateChannels || numSamples < 0 || numSamples > 0x7fffffff)
        return;

    const int64_t audioBytes = numSamples * numCh * (int64_t) sizeof(float);
    if (audioBytes > (int64_t) (in.getTotalLength() - in.getPosition()))
        return;   // truncated / corrupt state -- don't try to read past the end

    juce::AudioBuffer<float> loaded(numCh, (int) numSamples);
    for (int ch = 0; ch < numCh; ++ch)
        in.read(loaded.getWritePointer(ch), (int) ((size_t) numSamples * sizeof(float)));

    // Slice markers are not restored from state -- they're session-only (see getStateInformation).
    // Any trailing marker bytes an older blob wrote are simply left unread.

    document.newEmptyDocument(numCh, sr);
    document.beginChange();
    document.commitChange(std::move(loaded), "Load State");
    document.loopStart = lStart;
    document.loopEnd = lEnd;
    document.loopEnabled = lEnabled;
    document.loopPingPong = lPingPong;
    document.loopReverse = lReverse;
    document.setSelection(selStartS, selEndS);   // clamps to the loaded length

    document.playbackSpeed.store(juce::jlimit(AudioDocument::kMinSpeed, AudioDocument::kMaxSpeed, spd > 0.0 ? spd : 1.0));
    document.playbackPitch.store(juce::jlimit(AudioDocument::kMinPitch, AudioDocument::kMaxPitch, pch));
    document.playbackStretch.store(juce::jlimit(AudioDocument::kMinStretch, AudioDocument::kMaxStretch, str > 0.0 ? str : 1.0));
    document.autoRecordThresholdDb.store(juce::jlimit(-60.0, 0.0, thresh));
    document.filterBase.store(juce::jlimit(0.0, 1.0, fBase));
    document.filterWidth.store(juce::jlimit(0.0, 1.0, fWidth));
    document.filterHpQ.store(juce::jlimit(0.0, 1.0, fHpQ));
    document.filterLpQ.store(juce::jlimit(0.0, 1.0, fLpQ));
    document.playbackGainDb.store(juce::jlimit(AudioDocument::kMinGainDb, AudioDocument::kMaxGainDb, gDb));
    document.filterModel.store(juce::jlimit(0, 1, fModel));
    document.loopCrossfadeMs.store(juce::jlimit(0.0, 200.0, loopXfadeMs));
    document.bakeLoopCrossfadeOnExport.store(loopXfadeBake);

    document.numVisibleLfos.store(juce::jmax(1, loadedNumLfos));
    for (int i = 0; i < AudioDocument::kMaxLfos; ++i)
    {
        auto& dst = document.lfoSlots[i];
        const LoadedLfo s = (i < loadedNumLfos) ? loadedLfos[i] : LoadedLfo{};   // beyond loadedNumLfos: reset to defaults
        dst.enabled.store(s.enabled);
        dst.shape.store(juce::jlimit(0, (int) r3wrk::LfoShape::sampleHold, s.shape));
        dst.rateRange.store(juce::jlimit(0, (int) r3wrk::LfoRateRange::audio, s.rateRange));
        dst.rate01.store(juce::jlimit(0.0, 1.0, s.rate01));
        dst.target.store(juce::jlimit(0, r3wrk::kNumModTargets - 1, s.target));
        dst.amount.store(juce::jlimit(-1.0, 1.0, s.amount));
        dst.tempoSync.store(s.tempoSync);
    }
    document.reverbEnabled.store(rvEnabled);
    document.reverbSize.store(juce::jlimit(0.0, 1.0, rvSize));
    document.reverbAbsorb.store(juce::jlimit(0.0, 1.0, rvAbsorb));
    document.reverbDecay.store(juce::jlimit(0.0, 1.0, rvDecay));
    document.reverbTilt.store(juce::jlimit(0.0, 1.0, rvTilt));
    document.reverbMix.store(juce::jlimit(0.0, 1.0, rvMix));
    document.reverbPredelay.store(juce::jlimit(0.0, 1.0, rvPredelay));
    document.reverbWidth.store(juce::jlimit(0.0, 1.0, rvWidth));

    document.plexEnabled.store(pxEnabled);
    document.plexLevel.store(juce::jlimit(0.0, 1.0, pxLevel));
    document.plexPlexus.store(juce::jlimit(0.0, 1.0, pxPlexus));
    document.plexSize.store(juce::jlimit(0.0, 1.0, pxSize));
    document.plexDiffuse.store(juce::jlimit(0.0, 1.0, pxDiffuse));
    document.plexDecay.store(juce::jlimit(0.0, 1.0, pxDecay));
    document.plexColor.store(juce::jlimit(0.0, 1.0, pxColor));
    document.plexMix.store(juce::jlimit(0.0, 1.0, pxMix));

    document.mimeoEnabled.store(mxEnabled);
    document.mimeoZone.store(juce::jlimit(0.0, 1.0, mxZone));
    document.mimeoRate.store(juce::jlimit(0.0, 1.0, mxRate));
    document.mimeoRepeats.store(juce::jlimit(0.0, 1.0, mxRepeats));
    document.mimeoColor.store(juce::jlimit(0.0, 1.0, mxColor));
    document.mimeoHalo.store(juce::jlimit(0.0, 1.0, mxHalo));
    document.mimeoMix.store(juce::jlimit(0.0, 1.0, mxMix));
    document.mimeoSkew.store(juce::jlimit(0.0, 1.0, mxSkew));
    document.mimeoPingPong.store(mxPingPong);

    document.setSourceFilePath(sourceFilePath);   // "" on an older state blob -- header shows "Untitled"

    document.clearSliceMarkers();   // session-only; a restored document starts with no markers

    document.markAsOriginal();   // the restored session is the new "Revert to Original" baseline
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new R3WRKAudioProcessor();
}
