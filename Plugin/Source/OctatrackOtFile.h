#pragma once
#include <JuceHeader.h>
#include <vector>

/**
    Builds an Elektron Octatrack ".ot" sample-attributes file for a sliced sample chain,
    matching the layout the open-source OctaChainer tool writes (which the Octatrack loads):

      - always exactly 832 bytes
      - every multi-byte integer is big-endian
      - 16-byte magic "FORM\0\0\0\0DPS1SMPA", then 7 reserved bytes 00 00 00 00 00 02 00
      - tempo (BPM*24), trim/loop length + start/end + loop point (all in samples),
        stretch/loop mode, gain, trig quantize
      - 64 fixed slice slots, each { uint32 startSample, uint32 endSample, uint32 loopPoint };
        unused slots are all-zero, used ones get loopPoint = 0xFFFFFFFF ("no loop")
      - uint32 sliceCount
      - uint16 checksum = (sum of every byte from offset 0x10 to 0x33D) & 0xFFFF

    `slices` are half-open [start, end) sample ranges within a clip of `totalSamples`
    samples; at most 64 are written (the Octatrack's hard limit) and the caller is expected
    to have clamped/sorted them. `bpm` only feeds the tempo field -- the slice offsets are
    exact sample positions regardless of it.

    Kept free of AudioDocument/GUI types so the headless SmokeTest can exercise it directly.
*/
namespace OctatrackOtFile
{
    static constexpr int kFileSize   = 832;
    static constexpr int kMaxSlices  = 64;

    juce::MemoryBlock build (int64_t totalSamples,
                             const std::vector<juce::Range<int64_t>>& slices,
                             double bpm = 120.0);

    /// Convenience: build() then write the bytes to `otFile`, overwriting. Returns false on
    /// a write failure or if there's nothing to slice.
    bool writeToFile (const juce::File& otFile,
                      int64_t totalSamples,
                      const std::vector<juce::Range<int64_t>>& slices,
                      double bpm = 120.0);
}
