#pragma once
#include "AudioDocument.h"

/** A tiny process-wide clipboard for cut/copy/paste between edits. */
struct Clipboard
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    bool hasContent() const { return buffer.getNumSamples() > 0; }
};

/**
    Stateless helper functions that mutate an AudioDocument as a single
    undoable step. Each function calls doc.beginChange()/commitChange()
    internally, so callers just call e.g. EditActions::cut(doc, clipboard).
*/
namespace EditActions
{
    void cut(AudioDocument& doc, Clipboard& clipboard);
    void copy(const AudioDocument& doc, Clipboard& clipboard);
    void pasteReplace(AudioDocument& doc, const Clipboard& clipboard); // replaces selection (or inserts at playhead if none)
    void pasteInsert(AudioDocument& doc, const Clipboard& clipboard, int64_t atSample);
    void deleteSelection(AudioDocument& doc);
    void trimToSelection(AudioDocument& doc);
    void insertSilence(AudioDocument& doc, int64_t atSample, int64_t numSamples);

    void normalize(AudioDocument& doc, float targetPeakDb = -0.3f);
    void applyGainDb(AudioDocument& doc, float gainDb);
    void fadeIn(AudioDocument& doc);
    void fadeOut(AudioDocument& doc);
    void reverse(AudioDocument& doc);
    void silence(AudioDocument& doc);

    // Replace the current effective range with newRegion (used by time-stretch/pitch-shift).
    void replaceRangeWith(AudioDocument& doc, juce::Range<int64_t> range, const juce::AudioBuffer<float>& newRegion,
                           const juce::String& actionName);

    // Writes each region between consecutive chop markers (and the tail) to its own wav file
    // in destFolder, named baseName_001.wav, baseName_002.wav, ...
    bool exportChopSlices(const AudioDocument& doc, const juce::File& destFolder, const juce::String& baseName);
}
