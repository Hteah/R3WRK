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

    // Writes the current effective range (the selection, or the whole clip if none) to `file`
    // as a 24-bit WAV.
    bool exportSelection(const AudioDocument& doc, const juce::File& file);

    //==============================================================================
    // Slice export (see AudioDocument's slice-marker section).

    // Writes each region between the document's slice markers as its own 24-bit WAV into
    // `folder` (created if needed), named "<baseName> 01.wav", "<baseName> 02.wav", ...
    // Returns the number of files written (0 if there are no slice markers).
    int sliceToFolder(const AudioDocument& doc, const juce::File& folder, const juce::String& baseName);

    // Writes an Octatrack sample chain: the whole clip to `wavFile` as a 24-bit WAV, plus a
    // sibling "<wavFile without extension>.ot" carrying the slice points (see OctatrackOtFile).
    // Falls back to a single whole-clip slice if there are no markers. Caps at the Octatrack's
    // 64-slice limit. `bpm` only feeds the .ot's tempo field. Returns false on a write failure.
    bool exportOctatrackChain(const AudioDocument& doc, const juce::File& wavFile, double bpm = 120.0);
}
