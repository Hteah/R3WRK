#include "OctatrackOtFile.h"

namespace OctatrackOtFile
{
namespace
{
    struct Writer
    {
        juce::MemoryBlock block;
        int pos = 0;

        explicit Writer (int size) : block ((size_t) size, true) {}

        void bytes (const uint8_t* src, int n)
        {
            std::memcpy (static_cast<uint8_t*> (block.getData()) + pos, src, (size_t) n);
            pos += n;
        }
        void u32be (uint32_t v)
        {
            const uint8_t b[4] { (uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
            bytes (b, 4);
        }
        void u16be (uint16_t v)
        {
            const uint8_t b[2] { (uint8_t) (v >> 8), (uint8_t) v };
            bytes (b, 2);
        }
        void u8 (uint8_t v) { bytes (&v, 1); }
    };
}

juce::MemoryBlock build (int64_t totalSamples,
                         const std::vector<juce::Range<int64_t>>& slicesIn,
                         double bpm)
{
    const auto clampU32 = [] (int64_t v) -> uint32_t
    {
        return (uint32_t) juce::jlimit ((int64_t) 0, (int64_t) 0xFFFFFFFFll, v);
    };

    // At most 64 slots; caller is expected to have sorted/clamped, but be defensive.
    std::vector<juce::Range<int64_t>> slices;
    for (auto r : slicesIn)
    {
        if ((int) slices.size() >= kMaxSlices)
            break;
        const auto s = juce::jlimit ((int64_t) 0, totalSamples, r.getStart());
        const auto e = juce::jlimit (s,               totalSamples, r.getEnd());
        if (e > s)
            slices.push_back ({ s, e });
    }

    Writer w (kFileSize);

    // 0x00: magic
    const uint8_t magic[16] { 0x46,0x4F,0x52,0x4D, 0x00,0x00,0x00,0x00,
                              0x44,0x50,0x53,0x31, 0x53,0x4D,0x50,0x41 };
    w.bytes (magic, 16);

    // 0x10: 7 reserved bytes
    const uint8_t reserved[7] { 0x00,0x00,0x00,0x00,0x00,0x02,0x00 };
    w.bytes (reserved, 7);

    // 0x17 tempo (BPM * 24), then length/mode/gain fields.
    w.u32be ((uint32_t) juce::roundToInt (juce::jlimit (30.0, 300.0, bpm) * 24.0));
    w.u32be (clampU32 (totalSamples));   // 0x1B trim_len  -- whole clip
    w.u32be (clampU32 (totalSamples));   // 0x1F loop_len  -- whole clip
    w.u32be (0);                         // 0x23 stretch   -- off
    w.u32be (0);                         // 0x27 loop      -- off
    w.u16be (0x0030);                    // 0x2B gain      -- 0 dB
    w.u8    (0xFF);                      // 0x2D quantize  -- direct
    w.u32be (0);                         // 0x2E trim_start
    w.u32be (clampU32 (totalSamples));   // 0x32 trim_end
    w.u32be (0);                         // 0x36 loop_point

    // 0x3A: 64 slice slots.
    for (int i = 0; i < kMaxSlices; ++i)
    {
        if (i < (int) slices.size())
        {
            w.u32be (clampU32 (slices[(size_t) i].getStart()));
            w.u32be (clampU32 (slices[(size_t) i].getEnd()));
            w.u32be (0xFFFFFFFFu);   // per-slice loop point -- "no loop"
        }
        else
        {
            w.u32be (0);
            w.u32be (0);
            w.u32be (0);
        }
    }

    // 0x33A: slice count.
    w.u32be ((uint32_t) slices.size());

    // 0x33E: checksum = sum of bytes [0x10, 0x33D] & 0xFFFF.
    const auto* data = static_cast<const uint8_t*> (w.block.getData());
    uint32_t sum = 0;
    for (int i = 0x10; i <= 0x33D; ++i)
        sum += data[i];
    w.u16be ((uint16_t) (sum & 0xFFFF));

    jassert (w.pos == kFileSize);
    return std::move (w.block);
}

bool writeToFile (const juce::File& otFile,
                  int64_t totalSamples,
                  const std::vector<juce::Range<int64_t>>& slices,
                  double bpm)
{
    if (totalSamples <= 0 || slices.empty())
        return false;

    auto bytes = build (totalSamples, slices, bpm);
    otFile.deleteFile();
    return otFile.replaceWithData (bytes.getData(), bytes.getSize());
}

} // namespace OctatrackOtFile
