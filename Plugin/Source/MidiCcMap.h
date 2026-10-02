#pragma once
#include <cstdint>

/**
    R3WRK's fixed MIDI CC map -- "like any synth": every knob and button has its own CC number on
    the MIDI channel set in Tools > MIDI Channel (default 1). THE source of truth: the dispatcher
    (MidiCcDispatcher.h), the smoke test and the printed chart (`R3WRKSmokeTest
    --print-midi-chart`, tools/make_midi_chart.py) all read kMidiCcMap, so they can't drift.

    Knobs: CC 0..127 covers the knob's whole travel (the on-screen knob jumps to it), with CC 64
    exactly at the knob's centre (Pitch 0 st, Speed 1x, ...); CHO's are the MnM's raw 0..127. Buttons act
    on the press (value rising to 64+) and ignore the release -- momentary controller buttons
    (127 on press, 0 on release) flip a toggle once per press.

    Avoided CCs (reserved by convention on controllers / DAWs): 0 and 32 (bank select), 1 (mod
    wheel), 6 and 38 (data entry), 64-69 (pedals), 96-101 (data inc/dec, NRPN/RPN), 120-127
    (channel mode). CC 7 = volume is kept on purpose: it's Gain.
*/
namespace r3wrk::midi
{
    enum class Kind : uint8_t { knob, toggle, cycle, trigger };

    enum class Ctl : uint8_t
    {
        gain,
        pitch, speed, stretch, dirt, base, width, hpQ, lpQ, start, end,
        dirtRate, dirtBits,
        choDel, choDep, choSpd, choMix, choFb, choWid, choLp, choInp,
        rtrgTime, rtrgFade,
        dlyZone, dlyRate, dlyRepeats, dlyColor, dlyHalo, dlyMix, dlySkew,
        plxLevel, plxPlexus, plxSize, plxDiffuse, plxDecay, plxColor, plxMix, plxCouple, plxSkew,
        rvbSize, rvbAbsorb, rvbDecay, rvbTilt, rvbMix, rvbPredelay, rvbWidth,
        shmSize, shmDecay, shmTone, shmAmount, shmMovement, shmWidth, shmMix,
        overdubLevel, overdubFeedback, loopCrossfade, autoRecThreshold,
        // buttons
        playStop, playFromStart, record, loopMode, overdub, monitor, autoRecord,
        filterOn, filterModel, slot1Switch, slot1On, slot2Switch, slot2On, slot3Switch, slot3On,
        shmFreeze, shmInterval, dlyPingPong, rtrgSync, monitorFx,
        scrubMode, sliceMode, recordDesktop, captureOutput, clear, reset
    };

    struct Entry
    {
        int cc;
        Ctl ctl;
        Kind kind;
        const char* section;
        const char* name;
        const char* detail;   // the chart's "range / behaviour" column
    };

    inline constexpr Entry kMidiCcMap[] =
    {
        { 7,   Ctl::gain,        Kind::knob,   "Output",   "Gain",            "-inf .. +12 dB (master volume, last in the chain)" },

        { 14,  Ctl::pitch,       Kind::knob,   "Knob row", "Pitch",           "-12 .. +12 semitones (64 = 0)" },
        { 15,  Ctl::speed,       Kind::knob,   "Knob row", "Speed",           "0.25x .. 4x tape speed (64 = 1x)" },
        { 16,  Ctl::stretch,     Kind::knob,   "Knob row", "Stretch",         "0.25x .. 50x (64 = 1x)" },
        { 17,  Ctl::dirt,        Kind::knob,   "Knob row", "Dirt (Drive)",    "off .. 100%" },
        { 18,  Ctl::base,        Kind::knob,   "Knob row", "Base",            "filter high-pass corner (0 = off)" },
        { 19,  Ctl::width,       Kind::knob,   "Knob row", "Width",           "filter low-pass above Base (127 = open)" },
        { 20,  Ctl::hpQ,         Kind::knob,   "Knob row", "HP Q",            "0 .. 100% resonance" },
        { 21,  Ctl::lpQ,         Kind::knob,   "Knob row", "LP Q",            "0 .. 100% resonance" },
        { 22,  Ctl::start,       Kind::knob,   "Knob row", "Start",           "selection start (slides the whole selection)" },
        { 23,  Ctl::end,         Kind::knob,   "Knob row", "End",             "selection end" },
        { 24,  Ctl::dirtRate,    Kind::knob,   "Knob row", "Dirt Rate",       "sample-rate reduction (127 = clean)" },
        { 25,  Ctl::dirtBits,    Kind::knob,   "Knob row", "Dirt Bits",       "bit reduction (127 = clean)" },

        { 40,  Ctl::choDel,      Kind::knob,   "CHO",      "DEL",             "0 .. 127 (same as the MnM)" },
        { 41,  Ctl::choDep,      Kind::knob,   "CHO",      "DEP",             "0 .. 127" },
        { 42,  Ctl::choSpd,      Kind::knob,   "CHO",      "SPD",             "0 .. 127" },
        { 43,  Ctl::choMix,      Kind::knob,   "CHO",      "MIX",             "0 .. 127" },
        { 44,  Ctl::choFb,       Kind::knob,   "CHO",      "FB",              "0 .. 127" },
        { 45,  Ctl::choWid,      Kind::knob,   "CHO",      "WID",             "0 .. 127" },
        { 46,  Ctl::choLp,       Kind::knob,   "CHO",      "LP",              "0 .. 127" },
        { 47,  Ctl::choInp,      Kind::knob,   "CHO",      "INP",             "0 .. 127" },
        { 48,  Ctl::rtrgTime,    Kind::knob,   "RTRG",     "Time",            "note value (Sync) or 10 ms .. 1 s (Free)" },
        { 49,  Ctl::rtrgFade,    Kind::knob,   "RTRG",     "Fade",            "64 = plain repeats, below fades out, above builds up" },

        { 50,  Ctl::dlyZone,     Kind::knob,   "DLY",      "Zone",            "8 zones across the travel" },
        { 51,  Ctl::dlyRate,     Kind::knob,   "DLY",      "Rate",            "0 .. 100%" },
        { 52,  Ctl::dlyRepeats,  Kind::knob,   "DLY",      "Repeats",         "0 .. 100%" },
        { 53,  Ctl::dlyColor,    Kind::knob,   "DLY",      "Color",           "64 = neutral" },
        { 54,  Ctl::dlyHalo,     Kind::knob,   "DLY",      "Halo",            "0 .. 100%" },
        { 55,  Ctl::dlyMix,      Kind::knob,   "DLY",      "Mix",             "0 .. 100%" },
        { 56,  Ctl::dlySkew,     Kind::knob,   "DLY",      "Skew",            "64 = centre" },

        { 57,  Ctl::plxLevel,    Kind::knob,   "PLX",      "Level",           "0 .. 100%" },
        { 58,  Ctl::plxPlexus,   Kind::knob,   "PLX",      "Plexus",          "0 .. 100%" },
        { 59,  Ctl::plxSize,     Kind::knob,   "PLX",      "Size",            "0 .. 100%" },
        { 60,  Ctl::plxDiffuse,  Kind::knob,   "PLX",      "Diffuse",         "0 .. 100%" },
        { 61,  Ctl::plxDecay,    Kind::knob,   "PLX",      "Decay",           "0 .. 100%" },
        { 62,  Ctl::plxColor,    Kind::knob,   "PLX",      "Color",           "64 = neutral" },
        { 63,  Ctl::plxMix,      Kind::knob,   "PLX",      "Mix",             "0 .. 100%" },
        { 70,  Ctl::plxCouple,   Kind::knob,   "PLX",      "Couple",          "0 .. 100%" },
        { 71,  Ctl::plxSkew,     Kind::knob,   "PLX",      "Skew",            "64 = centre" },

        { 72,  Ctl::rvbSize,     Kind::knob,   "RVB",      "Size",            "0 .. 100%" },
        { 73,  Ctl::rvbAbsorb,   Kind::knob,   "RVB",      "Absorb",          "0 .. 100%" },
        { 74,  Ctl::rvbDecay,    Kind::knob,   "RVB",      "Decay",           "0 .. 100%" },
        { 75,  Ctl::rvbTilt,     Kind::knob,   "RVB",      "Tilt",            "64 = flat" },
        { 76,  Ctl::rvbMix,      Kind::knob,   "RVB",      "Mix",             "0 .. 100%" },
        { 77,  Ctl::rvbPredelay, Kind::knob,   "RVB",      "Pre-delay",       "7 .. 500 ms" },
        { 78,  Ctl::rvbWidth,    Kind::knob,   "RVB",      "Width",           "0 .. 200% (64 = 100%)" },

        { 79,  Ctl::shmSize,     Kind::knob,   "SHM",      "Size",            "0 .. 100%" },
        { 80,  Ctl::shmDecay,    Kind::knob,   "SHM",      "Decay",           "0.3 .. 20 s" },
        { 81,  Ctl::shmTone,     Kind::knob,   "SHM",      "Tone",            "below 64 low-pass, above high-pass" },
        { 82,  Ctl::shmAmount,   Kind::knob,   "SHM",      "Shimmer",         "0 .. 100%" },
        { 83,  Ctl::shmMovement, Kind::knob,   "SHM",      "Movement",        "0 .. 100%" },
        { 84,  Ctl::shmWidth,    Kind::knob,   "SHM",      "Width",           "0 .. 100%" },
        { 85,  Ctl::shmMix,      Kind::knob,   "SHM",      "Mix",             "0 .. 100%" },

        { 86,  Ctl::overdubLevel,     Kind::knob, "Overdub", "Level",          "-inf .. +6 dB" },
        { 87,  Ctl::overdubFeedback,  Kind::knob, "Overdub", "Feedback",       "0 .. 100%" },
        { 88,  Ctl::loopCrossfade,    Kind::knob, "Transport", "Loop crossfade", "0 .. 50 ms" },
        { 89,  Ctl::autoRecThreshold, Kind::knob, "Transport", "Auto-Record threshold", "-60 .. 0 dB" },

        { 102, Ctl::playStop,      Kind::trigger, "Transport", "Play / Stop",      "press: play, or stop (also stops a recording)" },
        { 103, Ctl::playFromStart, Kind::trigger, "Transport", "Play from start",  "press: restart from the beginning" },
        { 104, Ctl::record,        Kind::trigger, "Transport", "Record",           "press: start / stop recording" },
        { 105, Ctl::loopMode,      Kind::cycle,   "Transport", "Loop mode",        "press: off > loop > ping-pong > reverse > off" },
        { 106, Ctl::overdub,       Kind::toggle,  "Transport", "Overdub",          "press: start / stop an overdub pass" },
        { 107, Ctl::monitor,       Kind::toggle,  "Transport", "Monitor",          "press: on / off" },
        { 108, Ctl::autoRecord,    Kind::toggle,  "Transport", "Auto-Record",      "press: arm / disarm" },
        { 109, Ctl::filterOn,      Kind::toggle,  "Knob row",  "Filter on/off",    "press: on / off (knobs keep their settings)" },
        { 110, Ctl::filterModel,   Kind::cycle,   "Knob row",  "Filter model",     "press: OT <> MNM" },
        { 111, Ctl::slot1Switch,   Kind::cycle,   "Slot 1",    "RTRG <> CHO",      "press: switch the slot" },
        { 112, Ctl::slot1On,       Kind::toggle,  "Slot 1",    "Slot 1 on",        "press: RTRG latch / CHO on-off" },
        { 113, Ctl::slot2Switch,   Kind::cycle,   "Slot 2",    "DLY <> PLX",       "press: switch the slot" },
        { 114, Ctl::slot2On,       Kind::toggle,  "Slot 2",    "Slot 2 on",        "press: on / off (off lets the tail ring out)" },
        { 115, Ctl::slot3Switch,   Kind::cycle,   "Slot 3",    "RVB <> SHM",       "press: switch the slot" },
        { 116, Ctl::slot3On,       Kind::toggle,  "Slot 3",    "Slot 3 on",        "press: on / off (off lets the tail ring out)" },
        { 117, Ctl::shmFreeze,     Kind::toggle,  "SHM",       "Freeze",           "press: on / off" },
        { 118, Ctl::shmInterval,   Kind::cycle,   "SHM",       "Interval",         "press: octave <> fifth" },
        { 119, Ctl::dlyPingPong,   Kind::toggle,  "DLY",       "Ping-Pong",        "press: on / off" },
        { 8,   Ctl::rtrgSync,      Kind::toggle,  "RTRG",      "Sync / Free",      "press: switch" },
        { 9,   Ctl::monitorFx,     Kind::toggle,  "Transport", "Monitor DRY / FX", "press: switch" },
        { 10,  Ctl::scrubMode,     Kind::toggle,  "Tools",     "Scrub mode",       "press: on / off (window open)" },
        { 11,  Ctl::sliceMode,     Kind::toggle,  "Tools",     "Slice mode",       "press: on / off (window open)" },
        { 12,  Ctl::recordDesktop, Kind::trigger, "Tools",     "Record Desktop",   "press: start / stop (Standalone, window open)" },
        { 13,  Ctl::captureOutput, Kind::trigger, "Tools",     "Capture Output",   "press: start / stop (Standalone, window open)" },
        { 26,  Ctl::clear,         Kind::trigger, "Tools",     "Clear",            "press: clear the waveform (window open)" },
        { 27,  Ctl::reset,         Kind::trigger, "Tools",     "Reset (bolt)",     "press: stop + every knob and effect to default" },
    };

    inline constexpr int kNumEntries = (int) (sizeof(kMidiCcMap) / sizeof(kMidiCcMap[0]));

    inline constexpr bool isReservedCc(int cc)
    {
        return cc == 0 || cc == 1 || cc == 6 || cc == 32 || cc == 38
            || (cc >= 64 && cc <= 69) || (cc >= 96 && cc <= 101) || cc >= 120;
    }
}
