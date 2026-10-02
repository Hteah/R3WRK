#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"
#include "ControlRanges.h"
#include "MidiCcMap.h"

/**
    Knob <-> AudioDocument binding for every knob in kMidiCcMap, in knob TRAVEL units (0..1 =
    the on-screen knob's own sweep, skews included). One place, used by both the MIDI CC
    dispatcher and the host parameters (HostParams.h), so a CC, an Ableton LFO and the mouse all
    move the same thing the same way.

    setKnob() for Start/End/Position changes the selection (AudioDocument::setSelection -> a change
    message), so it must run on the message thread; every other knob is a plain atomic store and
    is safe from any thread (host automation calls setValue on the audio thread).
*/
namespace r3wrk::midi
{
    inline std::atomic<double>* knobAtomic(AudioDocument& d, Ctl c)
    {
        switch (c)
        {
            case Ctl::gain:     return &d.playbackGainDb;
            case Ctl::pitch:    return &d.playbackPitch;
            case Ctl::speed:    return &d.playbackSpeed;
            case Ctl::stretch:  return &d.playbackStretch;
            case Ctl::dirt:     return &d.dirtDrive;
            case Ctl::base:     return &d.filterBase;
            case Ctl::width:    return &d.filterWidth;
            case Ctl::hpQ:      return &d.filterHpQ;
            case Ctl::lpQ:      return &d.filterLpQ;
            case Ctl::dirtRate: return &d.dirtRate;
            case Ctl::dirtBits: return &d.dirtBits;
            case Ctl::choDel: return &d.chorusDel;  case Ctl::choDep: return &d.chorusDep;
            case Ctl::choSpd: return &d.chorusSpd;  case Ctl::choMix: return &d.chorusMix;
            case Ctl::choFb:  return &d.chorusFb;   case Ctl::choWid: return &d.chorusWid;
            case Ctl::choLp:  return &d.chorusLp;   case Ctl::choInp: return &d.chorusInp;
            case Ctl::rtrgTime: return &d.rtrgTime;
            case Ctl::rtrgFade: return &d.rtrgFade;
            case Ctl::dlyZone: return &d.mimeoZone;     case Ctl::dlyRate: return &d.mimeoRate;
            case Ctl::dlyRepeats: return &d.mimeoRepeats; case Ctl::dlyColor: return &d.mimeoColor;
            case Ctl::dlyHalo: return &d.mimeoHalo;     case Ctl::dlyMix: return &d.mimeoMix;
            case Ctl::dlySkew: return &d.mimeoSkew;
            case Ctl::plxLevel: return &d.plexLevel;    case Ctl::plxPlexus: return &d.plexPlexus;
            case Ctl::plxSize: return &d.plexSize;      case Ctl::plxDiffuse: return &d.plexDiffuse;
            case Ctl::plxDecay: return &d.plexDecay;    case Ctl::plxColor: return &d.plexColor;
            case Ctl::plxMix: return &d.plexMix;        case Ctl::plxCouple: return &d.plexCouple;
            case Ctl::plxSkew: return &d.plexSkew;
            case Ctl::rvbSize: return &d.reverbSize;    case Ctl::rvbAbsorb: return &d.reverbAbsorb;
            case Ctl::rvbDecay: return &d.reverbDecay;  case Ctl::rvbTilt: return &d.reverbTilt;
            case Ctl::rvbMix: return &d.reverbMix;      case Ctl::rvbPredelay: return &d.reverbPredelay;
            case Ctl::rvbWidth: return &d.reverbWidth;
            case Ctl::shmSize: return &d.shimmerSize;   case Ctl::shmDecay: return &d.shimmerDecay;
            case Ctl::shmTone: return &d.shimmerTone;   case Ctl::shmAmount: return &d.shimmerAmount;
            case Ctl::shmMovement: return &d.shimmerMovement; case Ctl::shmWidth: return &d.shimmerWidth;
            case Ctl::shmMix: return &d.shimmerMix;
            case Ctl::overdubLevel: return &d.overdubLevel;
            case Ctl::overdubFeedback: return &d.overdubFeedback;
            case Ctl::loopCrossfade: return &d.loopCrossfadeMs;
            case Ctl::autoRecThreshold: return &d.autoRecordThresholdDb;
            default: return nullptr;   // Start / End / Position (the selection) and every button
        }
    }

    // The knobs whose stored value isn't already their 0..1 travel.
    inline const juce::NormalisableRange<double>* knobRange(Ctl c)
    {
        static const auto gain = ranges::gainDb(), pitch = ranges::pitch(), speed = ranges::speed(),
                          stretch = ranges::stretch(), base = ranges::filterBase(),
                          overdub = ranges::overdubLevel(), xfade = ranges::loopCrossfadeMs(),
                          thresh = ranges::autoRecordThresholdDb();
        switch (c)
        {
            case Ctl::gain: return &gain;           case Ctl::pitch: return &pitch;
            case Ctl::speed: return &speed;         case Ctl::stretch: return &stretch;
            case Ctl::base: return &base;           case Ctl::overdubLevel: return &overdub;
            case Ctl::loopCrossfade: return &xfade; case Ctl::autoRecThreshold: return &thresh;
            default: return nullptr;
        }
    }

    // Knob travel 0..1 -> the document. Message thread for Start/End (see the header comment).
    inline void setKnob(AudioDocument& d, Ctl c, double x)
    {
        x = juce::jlimit(0.0, 1.0, x);
        if (c == Ctl::start)    { d.setSelectionStartEdge(x); return; }
        if (c == Ctl::end)      { d.setSelectionEndEdge(x);   return; }
        if (c == Ctl::position) { d.setSelectionPosition(x);  return; }
        if (auto* a = knobAtomic(d, c))
        {
            const auto* r = knobRange(c);
            a->store(r != nullptr ? r->convertFrom0to1(x) : x);
        }
    }

    // The document -> knob travel 0..1 (the inverse of setKnob). Any thread.
    inline double getKnob(const AudioDocument& dc, Ctl c)
    {
        auto& d = const_cast<AudioDocument&>(dc);
        if (c == Ctl::position)
            return d.getSelectionPosition();
        if (c == Ctl::start || c == Ctl::end)
        {
            const double n = (double) juce::jmax((int64_t) 1, d.getNumSamples());
            return juce::jlimit(0.0, 1.0, (double) (c == Ctl::start ? d.getSelectionStart() : d.getSelectionEnd()) / n);
        }
        if (auto* a = knobAtomic(d, c))
        {
            const auto* r = knobRange(c);
            const double v = a->load();
            return juce::jlimit(0.0, 1.0, r != nullptr ? r->convertTo0to1(juce::jlimit(r->start, r->end, v)) : v);
        }
        return 0.0;
    }
}
