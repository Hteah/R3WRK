#pragma once
#include <JuceHeader.h>
#include <array>
#include <functional>
#include "AudioDocument.h"
#include "ControlRanges.h"
#include "MidiCcMap.h"

/**
    Applies incoming MIDI CCs (kMidiCcMap) on the MESSAGE thread. Owned by the processor and fed
    from its lock-free FIFO (see PluginProcessor::processBlock / timerCallback), so it works with
    the plugin window closed. Knobs and the drawer's toggles write straight into AudioDocument --
    the UI resyncs from there on its own timers, exactly as after an undo or a state load -- and
    each toggle mirrors the click code of the panel that owns it.

    Toolbar actions (Play, Record, Overdub, Loop, Scrub, ...) go to `uiAction` first: the editor's
    toolbar sets it and runs its real button, so MIDI does exactly what a click does. Without an
    editor, `fallbackAction` (the processor) handles the ones that make sense blind.

    Plain class, no processor dependency -- the smoke test drives it against a bare AudioDocument.
*/
namespace r3wrk::midi
{
    class MidiCcDispatcher
    {
    public:
        explicit MidiCcDispatcher(AudioDocument& d) : doc(d)
        {
            lastValue.fill(0);
            for (const auto& e : kMidiCcMap) byCc[(size_t) e.cc] = &e;
        }

        std::function<bool(Ctl)> uiAction;         // editor's toolbar; true = handled
        std::function<void(Ctl)> fallbackAction;   // processor, when no editor handled it

        // One CC message (cc 0..127, value 0..127), already filtered to our channel.
        void handle(int cc, int value)
        {
            if (cc < 0 || cc > 127) return;
            value = juce::jlimit(0, 127, value);
            const int previous = lastValue[(size_t) cc];
            lastValue[(size_t) cc] = value;

            const Entry* e = byCc[(size_t) cc];
            if (e == nullptr) return;

            if (e->kind == Kind::knob)
            {
                setKnob(e->ctl, isChorus(e->ctl) ? value / 127.0 : knobPosition(value));
                return;
            }
            // Buttons: act on the press only -- the value rising to 64+. A release (0) or a
            // repeated 127 without a release in between does nothing.
            if (value >= 64 && previous < 64)
                press(e->ctl);
        }

        // CC value -> knob travel 0..1 with CC 64 landing EXACTLY on the centre (Pitch 0 st, Speed
        // 1x, Color/Skew/Tilt neutral), 0 and 127 on the ends -- like most synths. (A plain
        // value/127 puts 64 at 0.504: Pitch +0.09 st.)
        static double knobPosition(int value)
        {
            return value <= 64 ? value / 128.0 : 0.5 + (value - 64) / 126.0;
        }
        // CHO's eight are the Monomachine's own raw 0..127 values (0..1 = raw / 127), so they map 1:1.
        static bool isChorus(Ctl c) { return c >= Ctl::choDel && c <= Ctl::choInp; }

    private:
        AudioDocument& doc;
        std::array<int, 128> lastValue {};
        std::array<const Entry*, 128> byCc {};

        static void put(std::atomic<double>& a, double v) { a.store(v); }
        static void flip(std::atomic<bool>& a) { a.store(! a.load()); }

        void setKnob(Ctl c, double x)
        {
            switch (c)
            {
                case Ctl::gain:     put(doc.playbackGainDb,  ranges::gainDb().convertFrom0to1(x)); break;
                case Ctl::pitch:    put(doc.playbackPitch,   ranges::pitch().convertFrom0to1(x)); break;
                case Ctl::speed:    put(doc.playbackSpeed,   ranges::speed().convertFrom0to1(x)); break;
                case Ctl::stretch:  put(doc.playbackStretch, ranges::stretch().convertFrom0to1(x)); break;
                case Ctl::dirt:     put(doc.dirtDrive, x); break;
                case Ctl::base:     put(doc.filterBase, ranges::filterBase().convertFrom0to1(x)); break;
                case Ctl::width:    put(doc.filterWidth, x); break;
                case Ctl::hpQ:      put(doc.filterHpQ, x); break;
                case Ctl::lpQ:      put(doc.filterLpQ, x); break;
                case Ctl::start:    doc.setSelectionStartFraction(x); break;
                case Ctl::end:      doc.setSelectionEndFraction(x); break;
                case Ctl::dirtRate: put(doc.dirtRate, x); break;
                case Ctl::dirtBits: put(doc.dirtBits, x); break;

                case Ctl::choDel: put(doc.chorusDel, x); break;
                case Ctl::choDep: put(doc.chorusDep, x); break;
                case Ctl::choSpd: put(doc.chorusSpd, x); break;
                case Ctl::choMix: put(doc.chorusMix, x); break;
                case Ctl::choFb:  put(doc.chorusFb,  x); break;
                case Ctl::choWid: put(doc.chorusWid, x); break;
                case Ctl::choLp:  put(doc.chorusLp,  x); break;
                case Ctl::choInp: put(doc.chorusInp, x); break;
                case Ctl::rtrgTime: put(doc.rtrgTime, x); break;
                case Ctl::rtrgFade: put(doc.rtrgFade, x); break;

                case Ctl::dlyZone:    put(doc.mimeoZone, x); break;
                case Ctl::dlyRate:    put(doc.mimeoRate, x); break;
                case Ctl::dlyRepeats: put(doc.mimeoRepeats, x); break;
                case Ctl::dlyColor:   put(doc.mimeoColor, x); break;
                case Ctl::dlyHalo:    put(doc.mimeoHalo, x); break;
                case Ctl::dlyMix:     put(doc.mimeoMix, x); break;
                case Ctl::dlySkew:    put(doc.mimeoSkew, x); break;

                case Ctl::plxLevel:   put(doc.plexLevel, x); break;
                case Ctl::plxPlexus:  put(doc.plexPlexus, x); break;
                case Ctl::plxSize:    put(doc.plexSize, x); break;
                case Ctl::plxDiffuse: put(doc.plexDiffuse, x); break;
                case Ctl::plxDecay:   put(doc.plexDecay, x); break;
                case Ctl::plxColor:   put(doc.plexColor, x); break;
                case Ctl::plxMix:     put(doc.plexMix, x); break;
                case Ctl::plxCouple:  put(doc.plexCouple, x); break;
                case Ctl::plxSkew:    put(doc.plexSkew, x); break;

                case Ctl::rvbSize:     put(doc.reverbSize, x); break;
                case Ctl::rvbAbsorb:   put(doc.reverbAbsorb, x); break;
                case Ctl::rvbDecay:    put(doc.reverbDecay, x); break;
                case Ctl::rvbTilt:     put(doc.reverbTilt, x); break;
                case Ctl::rvbMix:      put(doc.reverbMix, x); break;
                case Ctl::rvbPredelay: put(doc.reverbPredelay, x); break;
                case Ctl::rvbWidth:    put(doc.reverbWidth, x); break;

                case Ctl::shmSize:     put(doc.shimmerSize, x); break;
                case Ctl::shmDecay:    put(doc.shimmerDecay, x); break;
                case Ctl::shmTone:     put(doc.shimmerTone, x); break;
                case Ctl::shmAmount:   put(doc.shimmerAmount, x); break;
                case Ctl::shmMovement: put(doc.shimmerMovement, x); break;
                case Ctl::shmWidth:    put(doc.shimmerWidth, x); break;
                case Ctl::shmMix:      put(doc.shimmerMix, x); break;

                case Ctl::overdubLevel:     put(doc.overdubLevel, ranges::overdubLevel().convertFrom0to1(x)); break;
                case Ctl::overdubFeedback:  put(doc.overdubFeedback, x); break;
                case Ctl::loopCrossfade:    put(doc.loopCrossfadeMs, ranges::loopCrossfadeMs().convertFrom0to1(x)); break;
                case Ctl::autoRecThreshold: put(doc.autoRecordThresholdDb, ranges::autoRecordThresholdDb().convertFrom0to1(x)); break;
                default: break;
            }
        }

        void press(Ctl c)
        {
            switch (c)
            {
                // Drawer / knob-row toggles -- each mirrors its panel's click code.
                case Ctl::filterOn:    flip(doc.filterOn); break;
                case Ctl::filterModel: doc.filterModel.store((doc.filterModel.load() + 1) % 2); break;
                case Ctl::slot1Switch:   // RetrigPanel / ChorusPanel slot tabs
                    if (doc.fxSlotChorus.load()) doc.fxSlotChorus.store(false);
                    else { doc.rtrgLatched.store(false); doc.fxSlotChorus.store(true); }
                    break;
                case Ctl::slot1On:
                    if (doc.fxSlotChorus.load()) flip(doc.chorusEnabled);
                    else                         flip(doc.rtrgLatched);
                    break;
                case Ctl::slot2Switch:   // MimeophonPanel / PlexiphonPanel slot tabs
                {
                    const bool toPlex = doc.mimeoEnabled.load();
                    doc.plexEnabled.store(toPlex);
                    doc.mimeoEnabled.store(! toPlex);
                    break;
                }
                case Ctl::slot2On: flip(doc.delayOn); break;
                case Ctl::slot3Switch:   // ReverbPanel / ShimmerPanel slot tabs
                {
                    const bool toShimmer = doc.reverbEnabled.load();
                    doc.shimmerEnabled.store(toShimmer);
                    doc.reverbEnabled.store(! toShimmer);
                    break;
                }
                case Ctl::slot3On:     flip(doc.spaceOn); break;
                case Ctl::shmFreeze:   flip(doc.shimmerFreeze); break;
                case Ctl::shmInterval: flip(doc.shimmerFifth); break;
                case Ctl::dlyPingPong: flip(doc.mimeoPingPong); break;
                case Ctl::rtrgSync:    flip(doc.rtrgSync); break;
                case Ctl::monitor:     flip(doc.overdubMonitor); break;
                case Ctl::monitorFx:   flip(doc.overdubMonitorFx); break;
                case Ctl::autoRecord:  // EditorToolbar's Auto-Record button (disabled while recording)
                    if (doc.isRecording.load()) break;
                    flip(doc.autoRecordEnabled);
                    if (! doc.autoRecordEnabled.load()) doc.autoRecordTriggered = false;
                    doc.notifyChanged();
                    break;

                // Toolbar actions: the real buttons when the window is open, else the fallback.
                default:
                    if (! (uiAction && uiAction(c)) && fallbackAction)
                        fallbackAction(c);
                    break;
            }
        }
    };
}
