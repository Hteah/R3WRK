#pragma once
#include <JuceHeader.h>
#include "KnobBinding.h"
#include <optional>
#include <thread>

/**
    Every knob in kMidiCcMap as a host (DAW) parameter -- so Ableton's Map (LFO, Macro, Envelope
    Follower, automation lanes, ...) can grab "the knob you just turned" and drive it.

    No APVTS and no second copy of the values: getValue()/setValue() read and write the very
    AudioDocument atomics the UI and DSP already use (KnobBinding.h, knob-travel units), so the
    on-screen knob follows host modulation through its existing resync timers.

    The other direction -- telling the host when R3WRK's own knob moves (mouse, MIDI CC, the
    reset bolt) -- is a poll on the processor's message-thread timer (syncToHost): a value that
    differs from the last one the host set or was told about is reported with a begin/end change
    gesture, which is what Live's Map listens for. The host's own changes (an LFO writing
    setValue) update lastNotified first, so they are never echoed back as if the user had
    touched the knob (which would override the host's modulation/automation).

    Start/End move the selection, which isn't audio-thread safe, so a host write to them is
    parked in `pending` and applied by the same timer (applyPending).
*/
namespace r3wrk
{
    class DocKnobParam : public juce::AudioProcessorParameterWithID
    {
    public:
        DocKnobParam(AudioDocument& d, const midi::Entry& e)
            : juce::AudioProcessorParameterWithID(juce::ParameterID { e.id, 1 }, displayName(e)),
              doc(d), ctl(e.ctl), defaultValue((float) midi::getKnob(d, e.ctl))
        {
            lastNotified.store(defaultValue);
        }

        float getValue() const override { return (float) midi::getKnob(doc, ctl); }

        void setValue(float v) override   // host -> R3WRK (any thread, often the audio thread)
        {
            v = juce::jlimit(0.0f, 1.0f, v);
            lastNotified.store(v);
            // Our own report (syncToHost -> setValueNotifyingHost calls this): the value is
            // already in the document -- applying it again, parked for Start/End until the next
            // timer tick, re-set a hand-dragged Start edge to where it was 16 ms earlier while the
            // knob had slid the loop on (it widened moving right, shrank moving left).
            if (reporting.load() && std::this_thread::get_id() == reporterThread)
                return;
            // ...and the HOST's echo of it: Live (and JUCE's VST3 hosting) sends a value we
            // reported back into the plugin a moment later, from the audio thread. Same story --
            // re-applying it parked a stale Start edge while the knob slid on, so in the VST the
            // loop widened / shrank (the Standalone has no host). A host write equal to our last
            // report is that echo; the document already has (or has moved on from) that value.
            if (std::abs(v - lastReported.load()) < 1.0e-6f)
                return;
            if (isSelectionKnob())
            {
                // While R3WRK itself is moving the selection (a hand drag, or within 250 ms of
                // reporting one), a host write here can only be an echo -- late or out of order,
                // so it needn't match the last report. Ignore it. An LFO takes over again once
                // the hand lets go.
                if (doc.selectionEdgeDragging.load() != 0
                    || juce::Time::getMillisecondCounterHiRes() < doc.selectionLocalMoveUntilMs.load())
                    return;
                pending.store(v);
            }
            else
                midi::setKnob(doc, ctl, v);
        }

        float getDefaultValue() const override { return defaultValue; }

        juce::String getText(float v, int) const override
        {
            using C = midi::Ctl;
            const auto real = [this, v]
            {
                const auto* r = midi::knobRange(ctl);
                return r != nullptr ? r->convertFrom0to1(juce::jlimit(0.0f, 1.0f, v)) : (double) v;
            }();
            switch (ctl)
            {
                case C::pitch:   return (real > 0.0 ? "+" : "") + juce::String(real, 2) + " st";
                case C::speed:
                case C::stretch: return juce::String(real, 2) + "x";
                case C::gain:    return real <= AudioDocument::kMinGainDb + 0.05 ? juce::String("-inf dB")
                                                                               : juce::String(real, 1) + " dB";
                case C::overdubLevel:
                    return real < 0.001 ? juce::String("-inf dB") : juce::String(juce::Decibels::gainToDecibels(real), 1) + " dB";
                case C::loopCrossfade:    return juce::String(real, 1) + " ms";
                case C::autoRecThreshold: return juce::String(real, 1) + " dB";
                case C::choDel: case C::choDep: case C::choSpd: case C::choMix:
                case C::choFb:  case C::choWid: case C::choLp:  case C::choInp:
                    return juce::String(juce::roundToInt(v * 127.0f));   // the MnM's raw value
                default:         return juce::String(juce::roundToInt(v * 100.0f)) + "%";
            }
        }

        float getValueForText(const juce::String& text) const override
        {
            return juce::jlimit(0.0f, 1.0f, text.getFloatValue() / 100.0f);
        }

        // Message thread (the processor's timer).
        bool applyPending()   // true if a host write was applied AND moved the knob
        {
            const float v = pending.exchange(-1.0f);
            if (v < 0.0f) return false;
            const bool moved = std::abs(v - getValue()) > 1.0e-6f;
            midi::setKnob(doc, ctl, v);
            return moved;
        }

        // Report a change that didn't come from the host, as a held "touch" like a real knob drag:
        // the gesture begins at the first change, stays open while the value keeps moving, and
        // ends kGestureIdleMs after it stops. (An instant begin/end per change was invisible to
        // Ableton's Map -- it only catches a parameter that's being held.) nowMs: any monotonic ms.
        void syncToHost(double nowMs)
        {
            const bool skip = isSelectionKnob() && doc.isEmpty();   // no selection to report
            const float now = getValue();
            if (! skip && std::abs(now - lastNotified.load()) >= 1.0e-4f)
            {
                if (isSelectionKnob())   // R3WRK is moving the selection: hold off host echoes
                    doc.selectionLocalMoveUntilMs.store(nowMs + kGestureIdleMs);
                if (! inGesture) { beginChangeGesture(); inGesture = true; }
                reporterThread = std::this_thread::get_id();
                lastReported.store(now);
                reporting.store(true);
                setValueNotifyingHost(now);   // -> setValue(now): only updates lastNotified (see setValue)
                reporting.store(false);
                lastMoveMs = nowMs;
            }
            else if (inGesture && nowMs - lastMoveMs > kGestureIdleMs)
            {
                endChangeGesture();
                inGesture = false;
            }
        }
        static constexpr double kGestureIdleMs = 250.0;

        void forgetChanges() { lastNotified.store(getValue()); }   // after a state load: nothing to report
        bool hasUnreportedChange() const { return std::abs(getValue() - lastNotified.load()) >= 1.0e-4f; }
        midi::Ctl getCtl() const noexcept { return ctl; }

    private:
        static juce::String displayName(const midi::Entry& e)
        {
            const juce::String section(e.section);
            const juce::String name = juce::String(e.name).upToFirstOccurrenceOf(" (", false, false);
            if (section == "Knob row" || section == "Output" || section == "Transport")
                return name;
            return section + " " + name;
        }
        bool isSelectionKnob() const { return ctl == midi::Ctl::start || ctl == midi::Ctl::end || ctl == midi::Ctl::position; }

        AudioDocument& doc;
        const midi::Ctl ctl;
        const float defaultValue;
        std::atomic<float> lastNotified { 0.0f };
        std::atomic<float> pending { -1.0f };
        std::atomic<bool> reporting { false };    // inside syncToHost's setValueNotifyingHost
        std::atomic<float> lastReported { -1.0f };  // the last value we reported (its host echo is ignored)

        std::thread::id reporterThread;
        bool inGesture = false;     // message thread only (syncToHost)
        double lastMoveMs = 0.0;
    };

    /** Start, End and Position are three views of ONE selection: moving one moves the others. Only
        the one that was actually moved is reported to the host (otherwise Live would treat the
        others as touched -- breaking their automation, or letting Map grab the wrong one).
          - hostWrote: the host itself changed one of them this tick -> report none.
          - touched: the MIDI dispatcher moved this one -> report only it.
          - otherwise (the on-screen knobs / waveform brackets): Start, else End, else Position. */
    inline void keepOnlyTheMovedSelectionChange(DocKnobParam* start, DocKnobParam* end, DocKnobParam* position,
                                                bool hostWrote, std::optional<midi::Ctl> touched)
    {
        DocKnobParam* all[] = { start, end, position };
        DocKnobParam* keep = nullptr;
        if (! hostWrote)
        {
            if (touched.has_value())
            {
                for (auto* p : all) if (p != nullptr && p->getCtl() == *touched) keep = p;
            }
            else
            {
                for (auto* p : all) if (p != nullptr && p->hasUnreportedChange()) { keep = p; break; }
            }
        }
        for (auto* p : all)
            if (p != nullptr && p != keep) p->forgetChanges();
    }
}
