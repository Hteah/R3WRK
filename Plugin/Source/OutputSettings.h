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

    // "<folder>/R3WRK 2026-09-04 14.22.03[ selection].wav", guaranteed not to already exist.
    juce::File makeWavFile(bool isSelection);

    // The persisted Save format / sample rate / bit depth (defaults: WAV, keep rate, 24-bit).
    AudioSaveOptions saveOptions();
    void setSaveOptions(const AudioSaveOptions&);

private:
    juce::PropertiesFile& props();
    std::unique_ptr<juce::PropertiesFile> propsFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputSettings)
};
