#pragma once
#include <JuceHeader.h>
#include <array>
#include <functional>
#include <optional>
#include "AudioDocument.h"
#include "ControlRanges.h"
#include "MidiCcMap.h"
#include "KnobBinding.h"

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

        // Which selection knob (Start / End / Position) a CC last moved -- the host-parameter sync
        // reports only that one (keepOnlyTheMovedSelectionChange). Message thread; read-and-clear.
        std::optional<Ctl> takeSelectionTouch() { auto t = selectionTouch; selectionTouch.reset(); return t; }

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
        std::optional<Ctl> selectionTouch;

        static void flip(std::atomic<bool>& a) { a.store(! a.load()); }

        void setKnob(Ctl c, double x)   // KnobBinding.h
        {
            r3wrk::midi::setKnob(doc, c, x);
            if (c == Ctl::start || c == Ctl::end || c == Ctl::position)
                selectionTouch = c;   // see takeSelectionTouch()
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
