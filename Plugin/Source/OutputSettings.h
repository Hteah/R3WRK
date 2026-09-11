#pragma once
#include <JuceHeader.h>
#include "AudioDocument.h"   // AudioSaveOptions

/**
    The output folder that recordings and selection exports are auto-saved into,
    persisted (shared by every plugin instance + the standalone) next to the
    theme settings. Defaults to ~/Music/R3WRK until the user picks another.

    Also stores the "Save Options" (format / sample rate / bit depth) the Save and
    Save As commands write with.
*/
class OutputSettings
{
public:
    OutputSettings();
    ~OutputSettings();

    // The stored folder, or the default; the directory is created if it doesn't exist.
    juce::File folder();
    void setFolder(const juce::File&);

    // "<folder>/R3WRK 2026-09-04 14.22.03.wav", guaranteed not to already exist. Used for the
    // record auto-save; selection export builds its own "<source> [start-end]" name.
    juce::File makeWavFile();

    // The persisted Save format / sample rate / bit depth (defaults: WAV, keep rate, 24-bit).
    AudioSaveOptions saveOptions();
    void setSaveOptions(const AudioSaveOptions&);

    // Standalone "float on top" toggle, remembered across launches (default off).
    bool floatOnTop();
    void setFloatOnTop(bool);

    // Last "Insert Silence" duration in seconds, remembered across launches (default 0.5).
    double insertSilenceSecs();
    void setInsertSilenceSecs(double);

    // Black Box (VST/AU only) ring-buffer length in seconds -- 90 (R3WRKAudioProcessor::
    // kBlackBoxDurationShort) or 300/5 min (kBlackBoxDurationLong), remembered across launches
    // (default 5 min). A fresh plugin instance reads this once at construction; see
    // R3WRKAudioProcessor::setBlackBoxDurationSecs()'s header comment on why changing it isn't
    // kept live across instances already loaded.
    double blackBoxDurationSecs();
    void setBlackBoxDurationSecs(double secs);

private:
    juce::PropertiesFile& props();
    std::unique_ptr<juce::PropertiesFile> propsFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputSettings)
};
