// Headless correctness smoke test for the core editing engine (no GUI, no audio device).
// Exercises AudioDocument + EditActions + TimeStretchEngine directly.

#include <JuceHeader.h>
#include <cstring>
#include <complex>
#include "../Source/AudioDocument.h"
#include "../Source/EditActions.h"
#include "../Source/TimeStretchEngine.h"
#include "../Source/BiquadFilter.h"
#include "../Source/ReverbEngine.h"
#include "../Source/PlexiphonEngine.h"
#include "../Source/MimeophonEngine.h"
#include "../Source/Theme.h"
#include "../Source/DragScanRender.h"
#include "../Source/LofiStretch.h"
#include "../Source/DirtStage.h"
#include "../Source/OverdubWriter.h"
#include "../Source/RegionGather.h"
#include "../Source/AudioSafety.h"

namespace
{
    int failures = 0;

    void check(bool condition, const juce::String& what)
    {
        if (condition)
        {
            std::cout << "  [PASS] " << what << std::endl;
        }
        else
        {
            std::cout << "  [FAIL] " << what << std::endl;
            ++failures;
        }
    }

    void checkNear(double a, double b, double tol, const juce::String& what)
    {
        check(std::abs(a - b) <= tol, what + juce::String::formatted(" (got %.4f, expected ~%.4f)", a, b));
    }

    juce::AudioBuffer<float> makeSineBuffer(int numChannels, int numSamples, double sampleRate, double freqHz, float amplitude)
    {
        juce::AudioBuffer<float> buf(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = amplitude * (float) std::sin(2.0 * juce::MathConstants<double>::pi * freqHz * (double) i / sampleRate);
        }
        return buf;
    }

    void setDocumentContent(AudioDocument& doc, juce::AudioBuffer<float> content, double sampleRate)
    {
        doc.newEmptyDocument(content.getNumChannels(), sampleRate);
        doc.beginChange();
        doc.commitChange(std::move(content), "Init");
        doc.undoManager.clearUndoHistory();
    }
}

int main()
{
    const double sr = 44100.0;
    std::cout << "=== R3WRK core engine smoke test ===" << std::endl;

    // --- selection / copy / cut / undo ---------------------------------
    {
        std::cout << "-- selection, copy, cut, undo --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, (int) sr, sr, 440.0, 0.5f), sr);
        check(doc.getNumSamples() == (int64_t) sr, "document has 1 second of audio");

        doc.setSelection(1000, 2000);
        check(doc.getSelectionEnd() - doc.getSelectionStart() == 1000, "selection spans 1000 samples");

        Clipboard clip;
        EditActions::copy(doc, clip);
        check(clip.buffer.getNumSamples() == 1000, "copy captured 1000 samples");
        check(doc.getNumSamples() == (int64_t) sr, "copy did not change document length");

        int64_t beforeCut = doc.getNumSamples();
        EditActions::cut(doc, clip);
        check(doc.getNumSamples() == beforeCut - 1000, "cut removed 1000 samples");

        doc.undoManager.undo();
        check(doc.getNumSamples() == beforeCut, "undo restored original length");

        doc.undoManager.redo();
        check(doc.getNumSamples() == beforeCut - 1000, "redo re-applied the cut");
    }

    // --- each edit is its own undo step ------------------------------
    {
        std::cout << "-- each edit is its own undo step --" << std::endl;
        // juce::UndoManager keeps appending perform() calls to whatever transaction is
        // already open -- it only starts a fresh one right after construction or a
        // clearUndoHistory() call, never automatically past that -- so two commitChange()s
        // back to back, with no explicit beginNewTransaction() between them, would otherwise
        // silently merge into one undo step (see AudioDocument::commitChange()).
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 2000, sr, 440.0, 0.5f), sr);
        const float originalPeak = doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples());

        EditActions::applyGainDb(doc, -6.0f);
        const float halvedPeak = doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples());
        check(halvedPeak > 0.0f && halvedPeak < originalPeak, "first edit (gain) took effect");

        EditActions::silence(doc);
        check(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()) == 0.0f,
             "second edit (silence) took effect");

        doc.undoManager.undo();
        checkNear(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()), halvedPeak, 0.001,
                 "undoing once reverts only the silence, back to the gained (not original) peak");

        doc.undoManager.undo();
        checkNear(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()), originalPeak, 0.001,
                 "undoing again reverts the gain edit too, back to the original peak");
    }

    // --- revert to original -------------------------------------------
    {
        std::cout << "-- revert to original --" << std::endl;
        AudioDocument doc;
        // markAsOriginal() explicitly, the way loadFromFile()/stopRecording() do -- unlike
        // setDocumentContent() above, which is test-only scaffolding and doesn't call it.
        setDocumentContent(doc, makeSineBuffer(1, 2000, sr, 440.0, 0.5f), sr);
        doc.markAsOriginal();
        const float originalPeak = doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples());
        check(originalPeak > 0.05f, "original take is not silent");

        EditActions::silence(doc);
        check(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()) == 0.0f,
             "an edit after the original take can silence it");

        doc.revertToOriginal();
        check(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()) > 0.05f,
             "revertToOriginal restores the original take, not silence");

        // Reverting is itself an ordinary, undoable edit -- undo it and the silenced edit
        // comes back; redo puts the revert back.
        check(doc.undoManager.canUndo(), "the revert itself is undoable");
        doc.undoManager.undo();
        check(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()) == 0.0f,
             "undoing a revert restores the silenced edit");
        doc.undoManager.redo();

        // The bug this replaces: the old "Revert" walked undoManager.undo() in a loop, which
        // for a fresh recording undid the recording itself, wiping it to nothing. Reverting
        // again from an already-reverted (== original) buffer must stay at the original --
        // there's nothing further back for it to reach into.
        doc.revertToOriginal();
        check(doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples()) > 0.05f,
             "reverting again from the original stays at the original");
    }

    // --- paste ------------------------------------------------------------
    {
        std::cout << "-- paste --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 5000, sr, 440.0, 0.5f), sr);
        Clipboard clip;
        clip.buffer = makeSineBuffer(1, 300, sr, 220.0, 0.3f);
        clip.sampleRate = sr;

        doc.setSelection(1000, 1000); // zero-length -> insert
        EditActions::pasteReplace(doc, clip);
        check(doc.getNumSamples() == 5300, "paste-insert grew document by clipboard length");

        doc.setSelection(0, 5300);
        EditActions::pasteReplace(doc, clip); // replace whole selection with 300-sample clip
        check(doc.getNumSamples() == 300, "paste-replace over a selection shrinks to clipboard length");
    }

    // --- trim / delete / insert silence ------------------------------------
    {
        std::cout << "-- trim, delete, insert silence --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 10000, sr, 440.0, 0.5f), sr);

        doc.setSelection(2000, 4000);
        EditActions::trimToSelection(doc);
        check(doc.getNumSamples() == 2000, "trim keeps only the selection");

        doc.setSelection(0, 500);
        EditActions::deleteSelection(doc);
        check(doc.getNumSamples() == 1500, "delete removes the selection");

        EditActions::insertSilence(doc, 0, 100);
        check(doc.getNumSamples() == 1600, "insertSilence grows the document");
        float peak = doc.getBuffer().getMagnitude(0, 0, 100);
        check(peak == 0.0f, "inserted region is actually silent");
    }

    // --- gain / normalize / fade / reverse / silence -----------------------
    {
        std::cout << "-- gain, normalize, fade, reverse, silence --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 4410, sr, 440.0, 0.2f), sr);

        doc.clearSelection();
        EditActions::normalize(doc, -0.3f);
        float peakAfterNorm = doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples());
        checkNear((double) juce::Decibels::gainToDecibels(peakAfterNorm), -0.3, 0.05, "normalize hits target peak dB");

        auto beforeGainBuf = doc.getBuffer();
        doc.clearSelection();
        EditActions::applyGainDb(doc, -6.0f);
        float afterGainPeak = doc.getBuffer().getMagnitude(0, 0, (int) doc.getNumSamples());
        float beforeGainPeak = beforeGainBuf.getMagnitude(0, 0, beforeGainBuf.getNumSamples());
        checkNear((double) (afterGainPeak / beforeGainPeak), (double) juce::Decibels::decibelsToGain(-6.0f), 0.01,
                  "applyGainDb(-6dB) halves amplitude as expected");

        doc.clearSelection();
        EditActions::fadeIn(doc);
        check(std::abs(doc.getBuffer().getSample(0, 0)) < 1.0e-6f, "fadeIn starts at silence");

        auto beforeReverse = doc.getBuffer();
        doc.clearSelection();
        EditActions::reverse(doc);
        EditActions::reverse(doc);
        bool roundTripsOk = true;
        for (int i = 0; i < doc.getBuffer().getNumSamples() && roundTripsOk; ++i)
            if (std::abs(doc.getBuffer().getSample(0, i) - beforeReverse.getSample(0, i)) > 1.0e-6f)
                roundTripsOk = false;
        check(roundTripsOk, "reverse twice returns to the original signal");

        doc.setSelection(0, 1000);
        EditActions::silence(doc);
        check(doc.getBuffer().getMagnitude(0, 0, 1000) == 0.0f, "silence zeroes the selection");
    }

    // --- export selection --------------------------------------------------
    {
        std::cout << "-- export selection --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(2, (int) sr, sr, 440.0, 0.4f), sr); // 1 s stereo
        doc.setSelection(10000, 25000);   // 15000 samples

        auto out = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("r3wrk_export_selection.wav");
        out.deleteFile();
        bool ok = EditActions::exportSelection(doc, out);
        check(ok && out.existsAsFile(), "exportSelection wrote a file");

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(out));
        check(r != nullptr, "exported selection re-opens");
        if (r != nullptr)
        {
            check((int64_t) r->lengthInSamples == 15000, "exported file is exactly the selection length");
            check((int) r->numChannels == 2, "exported file kept the channel count");
        }
        check(doc.getNumSamples() == (int64_t) sr, "exportSelection did not modify the document");
        out.deleteFile();

        // With Save Options: honours the format + bit depth, same path as Save As.
        AudioSaveOptions o; o.format = AudioSaveOptions::Format::flac; o.bitDepth = 16;
        auto flac = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_export_sel.flac");
        flac.deleteFile();
        check(EditActions::exportSelection(doc, flac, o), "exportSelection writes FLAC when asked");
        {
            std::unique_ptr<juce::AudioFormatReader> rf(fm.createReaderFor(flac));
            check(rf != nullptr && (int64_t) rf->lengthInSamples == 15000, "the FLAC export is the selection length");
        }
        flac.deleteFile();

        // The Speed/Pitch/Stretch knobs are baked in: 2x Stretch -> ~2x as many samples.
        doc.playbackStretch.store(2.0);
        auto st = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_export_sel_2x.wav");
        st.deleteFile();
        check(EditActions::exportSelection(doc, st), "exportSelection with Stretch engaged writes a file");
        {
            std::unique_ptr<juce::AudioFormatReader> rs(fm.createReaderFor(st));
            check(rs != nullptr, "the stretched export re-opens");
            if (rs != nullptr)
                checkNear((double) rs->lengthInSamples, 30000.0, 1500.0,
                          "the stretched export is ~2x the selection length (knobs baked in)");
        }
        st.deleteFile();
        doc.playbackStretch.store(1.0);

        // Bake the loop crossfade into the export: armed + a non-zero amount -> the written
        // file's ends are faded (first/last samples near silence); off -> untouched.
        doc.loopCrossfadeMs.store(20.0);
        doc.bakeLoopCrossfadeOnExport.store(false);
        auto nofade = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_export_nofade.wav");
        nofade.deleteFile();
        check(EditActions::exportSelection(doc, nofade), "exportSelection (bake off) writes a file");
        {
            std::unique_ptr<juce::AudioFormatReader> rn(fm.createReaderFor(nofade));
            juce::AudioBuffer<float> b(1, 64);
            if (rn != nullptr) rn->read(&b, 0, 64, 0, true, false);
            check(rn != nullptr && b.getMagnitude(0, 0, 64) > 0.2f, "bake off -> the export starts at full level");
        }
        nofade.deleteFile();

        doc.bakeLoopCrossfadeOnExport.store(true);
        auto faded = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_export_faded.wav");
        faded.deleteFile();
        check(EditActions::exportSelection(doc, faded), "exportSelection (bake on) writes a file");
        {
            std::unique_ptr<juce::AudioFormatReader> rf(fm.createReaderFor(faded));
            if (rf != nullptr)
            {
                const int n = (int) rf->lengthInSamples;
                juce::AudioBuffer<float> head(1, 16), tail(1, 16), mid(1, 64);
                rf->read(&head, 0, 16, 0, true, false);
                rf->read(&tail, 0, 16, n - 16, true, false);
                rf->read(&mid, 0, 64, n / 2, true, false);
                check(head.getSample(0, 0) == 0.0f, "bake on -> exported file starts at exactly silence");
                check(head.getMagnitude(0, 0, 16) < mid.getMagnitude(0, 0, 64), "bake on -> the head is faded down");
                check(tail.getMagnitude(0, 0, 16) < mid.getMagnitude(0, 0, 64), "bake on -> the tail is faded down");
            }
            else check(false, "faded export re-opens");
        }
        faded.deleteFile();
        doc.loopCrossfadeMs.store(0.0);
        doc.bakeLoopCrossfadeOnExport.store(false);
    }

    // --- slice markers + slice/Octatrack export ----------------------------
    {
        std::cout << "-- slice markers + slice / Octatrack export --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 10000, sr, 440.0, 0.4f), sr);   // 10000 samples

        doc.addSliceMarker(2500);
        doc.addSliceMarker(7000);
        doc.addSliceMarker(2500);   // dupe -- should collapse
        doc.addSliceMarker(0);      // at the edge -- should be dropped
        doc.addSliceMarker(99999);  // past the end -- should be dropped
        check((int) doc.getSliceMarkers().size() == 2, "markers de-dupe and drop out-of-range");

        const auto regions = doc.getSliceRegions();
        check((int) regions.size() == 3, "2 markers -> 3 regions");
        check(regions[0].getStart() == 0 && regions[0].getEnd() == 2500, "first region is [0, m0)");
        check(regions[1].getStart() == 2500 && regions[1].getEnd() == 7000, "middle region is [m0, m1)");
        check(regions[2].getStart() == 7000 && regions[2].getEnd() == 10000, "last region is [m1, len)");

        // moveSliceMarker: reposition, reorder past a neighbour, merge onto another.
        // (markers are 2500, 7000 at this point)
        check(doc.moveSliceMarker(0, 500) == 0 && doc.getSliceMarkers()[0] == 500,
              "moveSliceMarker repositions, keeps order");
        check(doc.moveSliceMarker(0, 8000) == 1 && doc.getSliceMarkers()[1] == 8000,
              "moveSliceMarker past a neighbour reorders and returns the new index");
        check(doc.moveSliceMarker(1, 7000) == 0 && (int) doc.getSliceMarkers().size() == 1,
              "moveSliceMarker exactly onto another marker merges the two to one");

        // A length-changing edit clears the markers.
        doc.setSelection(0, 4000);
        EditActions::trimToSelection(doc);
        check(doc.getSliceMarkers().empty(), "a length-changing edit (trim) clears slice markers");

        // Re-mark on the trimmed 4000-sample clip and slice to a folder.
        doc.addSliceMarker(1000);
        doc.addSliceMarker(2000);
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_slices_test");
        dir.deleteRecursively();
        const int written = EditActions::sliceToFolder(doc, dir, "chunk");
        check(written == 3, "sliceToFolder wrote one WAV per region (3)");
        check(dir.getChildFile("chunk 01.wav").existsAsFile()
              && dir.getChildFile("chunk 02.wav").existsAsFile()
              && dir.getChildFile("chunk 03.wav").existsAsFile(), "slice files are 01/02/03-numbered");
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(dir.getChildFile("chunk 02.wav")));
            check(r != nullptr && r->lengthInSamples == 1000, "middle slice is exactly [1000, 2000)");
        }
        dir.deleteRecursively();

        // Octatrack chain: <name>.wav + <name>.ot.
        auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_ot_test.wav");
        auto ot  = wav.withFileExtension("ot");
        wav.deleteFile(); ot.deleteFile();
        check(EditActions::exportOctatrackChain(doc, wav, 120.0), "exportOctatrackChain succeeded");
        check(wav.existsAsFile() && ot.existsAsFile(), "chain wrote both .wav and .ot");

        juce::MemoryBlock otBytes;
        ot.loadFileAsData(otBytes);
        check(otBytes.getSize() == 832, "the .ot is exactly 832 bytes");
        const auto* b = static_cast<const uint8_t*>(otBytes.getData());
        check(b[0]==0x46 && b[1]==0x4F && b[2]==0x52 && b[3]==0x4D
              && b[8]==0x44 && b[9]==0x50 && b[10]==0x53 && b[11]==0x31
              && b[12]==0x53 && b[13]==0x4D && b[14]==0x50 && b[15]==0x41, "the .ot header is FORM....DPS1SMPA");

        const auto be32 = [b](int off)
        {
            return ((uint32_t) b[off] << 24) | ((uint32_t) b[off+1] << 16)
                 | ((uint32_t) b[off+2] << 8) | (uint32_t) b[off+3];
        };
        check(be32(0x17) == (uint32_t) (120 * 24), "tempo field is BPM*24");
        check(be32(0x2E) == 0 && be32(0x32) == 4000, "trim_start=0, trim_end=totalSamples");
        check(be32(0x33A) == 3, "slice_count is 3");
        check(be32(0x3A) == 0 && be32(0x3A + 4) == 1000, "slice 0 spans [0, 1000)");
        check(be32(0x3A + 12) == 1000 && be32(0x3A + 16) == 2000, "slice 1 spans [1000, 2000)");
        check(be32(0x3A + 24) == 2000 && be32(0x3A + 28) == 4000, "slice 2 spans [2000, 4000)");

        uint32_t sum = 0;
        for (int i = 0x10; i <= 0x33D; ++i) sum += b[i];
        const uint16_t storedChecksum = (uint16_t) (((uint32_t) b[0x33E] << 8) | b[0x33F]);
        check(storedChecksum == (uint16_t) (sum & 0xFFFF), "the .ot trailing checksum matches the summed body");

        wav.deleteFile(); ot.deleteFile();
    }

    // --- time-stretch / pitch-shift ------------------------------------------
    {
        std::cout << "-- time-stretch / pitch-shift (lofi engine: lengths) --" << std::endl;
        auto region = makeSineBuffer(1, (int) sr, sr, 440.0, 0.4f); // 1 second

        auto same = TimeStretchEngine::process(region, sr, 1.0, 0.0);
        checkNear((double) same.getNumSamples(), (double) region.getNumSamples(), sr * 0.02,
                  "ratio=1.0 keeps ~same length");

        auto longer = TimeStretchEngine::process(region, sr, 2.0, 0.0);
        checkNear((double) longer.getNumSamples(), (double) region.getNumSamples() * 2.0, sr * 0.05,
                  "ratio=2.0 roughly doubles length");

        auto shorter = TimeStretchEngine::process(region, sr, 0.5, 0.0);
        checkNear((double) shorter.getNumSamples(), (double) region.getNumSamples() * 0.5, sr * 0.05,
                  "ratio=0.5 roughly halves length");

        auto pitched = TimeStretchEngine::process(region, sr, 1.0, 12.0); // +1 octave, same length
        checkNear((double) pitched.getNumSamples(), (double) region.getNumSamples(), sr * 0.02,
                  "pitch-only shift keeps length constant");
        check(pitched.getMagnitude(0, 0, pitched.getNumSamples()) > 0.05f, "pitched output is not silent");
    }

    // --- lofi engine character (LofiStretch.h): Paulstretch keeps pitch, Speed is tape,
    // Pitch is granular + 14.7 kHz / 12-bit grit; never NaN, never silent, safe mid-stream moves
    {
        std::cout << "-- lofi engine: pitch, grit, stability --" << std::endl;
        auto zcFreq = [&](const juce::AudioBuffer<float>& b, int from, int to)
        {
            int crossings = 0;
            for (int i = from + 1; i < to; ++i)
                if ((b.getSample(0, i - 1) < 0.0f) != (b.getSample(0, i) < 0.0f)) ++crossings;
            return crossings / 2.0 / ((to - from) / sr);
        };
        auto finite = [](const juce::AudioBuffer<float>& b)
        {
            for (int c = 0; c < b.getNumChannels(); ++c)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    if (! std::isfinite(b.getSample(c, i))) return false;
            return true;
        };
        auto rms = [](const juce::AudioBuffer<float>& b, int from, int to) { return b.getRMSLevel(0, from, to - from); };
        const auto tone = makeSineBuffer(2, (int) (sr * 2), sr, 440.0, 0.5f);
        const float toneRms = rms(tone, 0, tone.getNumSamples());

        const auto paul = TimeStretchEngine::process(tone, sr, 1.0, 4.0, 0.0);
        const int pm0 = paul.getNumSamples() / 4, pm1 = paul.getNumSamples() * 3 / 4;
        check(finite(paul), "Paulstretch x4: no NaN/inf");
        checkNear(zcFreq(paul, pm0, pm1), 440.0, 440.0 * 0.03, "Paulstretch x4 keeps the 440 Hz pitch");
        check(rms(paul, pm0, pm1) > toneRms * 0.3f && rms(paul, pm0, pm1) < toneRms * 2.0f,
              juce::String::formatted("Paulstretch x4 level is sane (rms %.3f vs %.3f in)", rms(paul, pm0, pm1), toneRms));
        check(paul.getMagnitude(1, pm0, pm1 - pm0) > 0.05f, "Paulstretch runs the second channel too");

        const auto tape = TimeStretchEngine::process(tone, sr, 2.0, 1.0, 0.0);
        checkNear((double) tape.getNumSamples(), tone.getNumSamples() * 0.5, 2.0, "Speed x2 halves the length (tape)");
        checkNear(zcFreq(tape, tape.getNumSamples() / 4, tape.getNumSamples() * 3 / 4), 880.0, 880.0 * 0.03, "Speed x2 doubles the pitch (tape)");

        const auto up = TimeStretchEngine::process(tone, sr, 1.0, 1.0, 12.0);
        const int um0 = up.getNumSamples() / 4, um1 = up.getNumSamples() * 3 / 4;
        checkNear((double) up.getNumSamples(), (double) tone.getNumSamples(), 2.0, "Pitch +12 keeps the length");
        checkNear(zcFreq(up, um0, um1), 880.0, 880.0 * 0.06, "Pitch +12 lands near 880 Hz (granular, so roughly)");
        int held = 0, onGrid = 0;
        for (int i = um0 + 1; i < um1; ++i)
        {
            const float v = up.getSample(0, i);
            held += v == up.getSample(0, i - 1) ? 1 : 0;
            onGrid += std::abs(v * 2048.0f - std::round(v * 2048.0f)) < 1.0e-3f ? 1 : 0;
        }
        check(held > (um1 - um0) / 2, "Pitch grit: samples are held (14.7 kHz sample-and-hold)");
        check(onGrid == um1 - um0 - 1, "Pitch grit: every sample sits on the 12-bit grid");
        check(finite(up), "Pitch +12: no NaN/inf");

        const auto dry = TimeStretchEngine::process(tone, sr, 1.0, 1.0, 0.0);
        float maxDiff = 0.0f;
        for (int i = 0; i < dry.getNumSamples(); ++i) maxDiff = std::max(maxDiff, std::abs(dry.getSample(0, i) - tone.getSample(0, i)));
        check(maxDiff < 1.0e-6f, "all knobs centred: the engine passes audio through untouched");

        // Real-time use, as PluginProcessor drives it: 512-sample blocks, input on demand, and the
        // knobs moving mid-stream (speed 1->2, stretch 1->3 switching Paulstretch on, pitch 0->+7).
        r3wrk::LofiStretch eng;
        eng.prepare(sr, 2);
        eng.setParams(1.0, 1.0, 0.0);
        eng.reset();
        juce::AudioBuffer<float> inB(2, 8192), outB(2, 512);
        int64_t readPos = 0;
        float peak = 0.0f; bool ok = true; int silentBlocks = 0;
        for (int b = 0; b < (int) (6.0 * sr / 512); ++b)
        {
            const double t = b * 512 / sr;
            eng.setParams(1.0 + juce::jlimit(0.0, 1.0, t - 1.0), 1.0 + 2.0 * juce::jlimit(0.0, 1.0, t - 2.0), 7.0 * juce::jlimit(0.0, 1.0, t - 3.0));
            int produced = 0, guard = 5000;
            while (produced < 512 && --guard > 0)
            {
                if (eng.available() > 0)
                {
                    float* op[2] = { outB.getWritePointer(0, produced), outB.getWritePointer(1, produced) };
                    produced += eng.retrieve(op, 512 - produced);
                    continue;
                }
                const int req = juce::jlimit(1, 8192, eng.getSamplesRequired() > 0 ? eng.getSamplesRequired() : 256);
                for (int i = 0; i < req; ++i, ++readPos)
                    for (int c = 0; c < 2; ++c)
                        inB.setSample(c, i, tone.getSample(c, (int) (readPos % tone.getNumSamples())));
                const float* ip[2] = { inB.getReadPointer(0), inB.getReadPointer(1) };
                eng.process(ip, req, false);
            }
            ok = ok && produced == 512;
            for (int i = 0; i < 512; ++i) { ok = ok && std::isfinite(outB.getSample(0, i)); peak = std::max(peak, std::abs(outB.getSample(0, i))); }
            if (t > 3.5 && outB.getMagnitude(0, 0, 512) < 0.01f) ++silentBlocks;
        }
        check(ok, "real-time: every block filled, no NaN/inf while the knobs move");
        check(peak < 2.0f, juce::String::formatted("real-time: output stays bounded (peak %.2f)", peak));
        check(silentBlocks == 0, juce::String::formatted("real-time: no dropouts once everything is engaged (%d silent blocks)", silentBlocks));
    }

    // --- Dirt: Octatrack-style drive -> sample-rate -> bits, ahead of the filter (DirtStage.h)
    {
        std::cout << "-- Dirt (pre-filter drive / rate / bits) --" << std::endl;
        const int n = (int) sr;
        auto sine = [&](float amp) { return makeSineBuffer(1, n, sr, 440.0, amp); };
        auto run = [&](juce::AudioBuffer<float> b, double drive, double rate, double bits)
        {
            r3wrk::DirtStage d; d.prepare(sr); d.snapDrive(drive);
            d.process(b.getWritePointer(0), b.getNumSamples(), drive, rate, bits);
            return b;
        };
        auto goertzel = [&](const juce::AudioBuffer<float>& b, double f, int from)
        {
            const double w = 2 * juce::MathConstants<double>::pi * f / sr, c = 2 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (int i = from; i < b.getNumSamples(); ++i) { const double s0 = b.getSample(0, i) + c * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (b.getNumSamples() - from);
        };

        check(! r3wrk::DirtStage::engaged(0.0, 1.0, 1.0), "Dirt at default is not engaged (callers skip it: bit-exact bypass)");
        {
            auto in = sine(0.5f), out = run(in, 0.0, 1.0, 1.0);
            float diff = 0; for (int i = 0; i < n; ++i) diff = std::max(diff, std::abs(out.getSample(0, i) - in.getSample(0, i)));
            check(diff == 0.0f, "Drive 0, Rate/Bits off: output identical to input");
        }
        {
            auto in = sine(0.3f), out = run(in, 1.0, 1.0, 1.0);
            const int from = n / 4;
            const double f1 = goertzel(out, 440, from), f2 = goertzel(out, 880, from), f3 = goertzel(out, 1320, from);
            const double c2 = goertzel(in, 880, from), c3 = goertzel(in, 1320, from);
            check(f3 > 20 * (c3 + 1e-9) && f3 > f1 * 0.05, juce::String::formatted("Drive 1 adds odd harmonics (3f/f %.3f)", f3 / f1));
            check(f2 > 20 * (c2 + 1e-9) && f2 > f1 * 0.02, juce::String::formatted("Drive 1 adds even harmonics too -- the asymmetric clip (2f/f %.3f)", f2 / f1));
            double mean = 0; for (int i = from; i < n; ++i) mean += out.getSample(0, i);
            mean /= (n - from);
            check(std::abs(mean) < 0.005, juce::String::formatted("the DC blocker removes the bias offset (mean %.5f)", mean));
            check(out.getMagnitude(0, 0, n) <= 1.0f, juce::String::formatted("Drive output stays under the ceiling (peak %.3f)", out.getMagnitude(0, 0, n)));
        }
        {
            juce::AudioBuffer<float> hot(1, n);
            for (int i = 0; i < n; ++i) hot.setSample(0, i, (float) (4.0 * std::sin(2 * juce::MathConstants<double>::pi * 440 * i / sr)));
            auto out = run(hot, 1.0, 0.3, 0.2);
            bool finite = true; for (int i = 0; i < n; ++i) finite = finite && std::isfinite(out.getSample(0, i));
            check(finite && out.getMagnitude(0, 0, n) <= 2.0f, "+-4 input through everything: finite and within +-2");
        }
        {
            const double rate01 = std::log(4000.0 / 1000.0) / std::log(sr / 1000.0);   // ~4 kHz
            auto out = run(sine(0.5f), 0.0, rate01, 1.0);
            int runs = 0, len = 1; double total = 0;
            for (int i = 1; i < n; ++i)
            {
                if (out.getSample(0, i) == out.getSample(0, i - 1)) ++len;
                else { total += len; ++runs; len = 1; }
            }
            checkNear(total / juce::jmax(1, runs), sr / 4000.0, 0.6, "Rate ~4 kHz: held runs average ~11 samples");
        }
        {
            const double bits01 = (4 - 2) / 14.0;   // 4 bits
            check(r3wrk::DirtStage::bitDepth(bits01) == 4, "Bits knob maps to 4 bits");
            auto out = run(sine(0.7f), 0.0, 1.0, bits01);
            bool onGrid = true;
            for (int i = 0; i < n; ++i) { const float v = out.getSample(0, i) * 8.0f; onGrid = onGrid && std::abs(v - std::round(v)) < 1e-5f; }
            check(onGrid, "Bits 4: every sample sits on the 4-bit grid");
        }
        {
            // Drive swept 0 -> 1 -> 0 across a block-by-block run: the per-sample ramp means no
            // step bigger than a hard-clipped sine could make at full drive.
            auto b = sine(0.3f);
            r3wrk::DirtStage d; d.prepare(sr);
            for (int off = 0; off < n; off += 512)
            {
                const double t = (double) off / n;
                const double target = t < 0.5 ? (t < 0.1 ? 0.0 : 1.0) : 0.0;   // hard knob jumps
                d.process(b.getWritePointer(0, off), std::min(512, n - off), target, 1.0, 1.0);
            }
            float worst = 0; for (int i = 1; i < n; ++i) worst = std::max(worst, std::abs(b.getSample(0, i) - b.getSample(0, i - 1)));
            check(worst < 0.25f, juce::String::formatted("Drive knob jumps are ramped, no clicks (worst step %.3f)", worst));
            check(! d.busy() || b.getMagnitude(0, n - 512, 512) > 0.0f, "Drive returns to 0 cleanly");
        }
        {
            // Save/Export bakes Dirt in (renderWithPlaybackKnobs), and leaves audio alone when it's off.
            AudioDocument doc;
            const auto src = sine(0.3f);
            const auto dry = doc.renderWithPlaybackKnobs(src);
            float dryDiff = 0; for (int i = 0; i < n; ++i) dryDiff = std::max(dryDiff, std::abs(dry.getSample(0, i) - src.getSample(0, i)));
            check(dryDiff == 0.0f, "export with Dirt off: audio unchanged");
            doc.dirtDrive.store(0.8);
            doc.dirtBits.store((6 - 2) / 14.0);
            const auto baked = doc.renderWithPlaybackKnobs(src);
            float diff = 0; for (int i = 0; i < n; ++i) diff = std::max(diff, std::abs(baked.getSample(0, i) - src.getSample(0, i)));
            check(diff > 0.05f, juce::String::formatted("export with Dirt on bakes it in (max change %.3f)", diff));
        }
    }

    // --- Overdub (sound-on-sound) writer: timing across wraps/directions, mixing, ceiling ---
    {
        std::cout << "-- Overdub (sound-on-sound) --" << std::endl;
        const int L = 22050, lat = 1000, block = 512;

        // Plays a loop whose read order is `posAt(k)` for `total` samples, feeding back the
        // output delayed by `lat` as the "input" (the round trip through speakers and mic), and
        // overdubs it at Level 1 / Feedback 1. An impulse at p must come back exactly onto p.
        auto roundTrip = [&](int p, int total, std::function<int64_t(int)> posAt, bool& onlyThere)
        {
            juce::AudioBuffer<float> doc(1, L); doc.clear();
            doc.setSample(0, p, 0.5f);
            std::vector<float> played;
            r3wrk::OverdubWriter w; w.prepare();
            juce::AudioBuffer<float> in(1, block);
            for (int k0 = 0; k0 < total; k0 += block)
            {
                const int n = std::min(block, total - k0);
                for (int i = 0; i < n; ++i)
                {
                    const int64_t pos = posAt(k0 + i);
                    w.pushPosition(pos);
                    played.push_back(doc.getSample(0, (int) pos));
                }
                for (int i = 0; i < n; ++i)
                {
                    const int k = k0 + i - lat;
                    in.setSample(0, i, k >= 0 ? played[(size_t) k] : 0.0f);
                }
                w.write(doc, in, n, lat, 1.0f, 1.0f);
            }
            onlyThere = true;
            for (int i = 0; i < L; ++i)
                if (i != p && doc.getSample(0, i) != 0.0f) onlyThere = false;
            return doc.getSample(0, p);
        };
        bool clean = false;
        float v = roundTrip(3000, 6000, [](int k) { return (int64_t) (k % L); }, clean);
        check(v == 1.0f && clean, juce::String::formatted("forward loop: the layer lands exactly in time (%.3f at p, clean elsewhere: %d)", v, clean));
        v = roundTrip(L - 300, L + 2000, [](int k) { return (int64_t) (k % L); }, clean);
        check(v == 1.0f && clean, "across a loop wrap: still exactly in time");
        v = roundTrip(5000, L, [](int k) { return (int64_t) (L - 1 - (k % L)); }, clean);
        check(v == 1.0f && clean, "reverse loop: exactly in time");
        // Ping-pong reads p twice per cycle (forward, then on the way back) and both passes get
        // overdubbed -- correct sound-on-sound. Stop after the first layer is written (which happens
        // while playback is already on the return leg) and before the second pass is heard.
        v = roundTrip(L - 200, L + 1000, [](int k) { const int per = 2 * L - 2, m = k % per; return (int64_t) (m < L ? m : per - m); }, clean);
        check(v == 1.0f && clean, "ping-pong, written while on the return leg: exactly in time");

        {
            juce::AudioBuffer<float> doc(2, 4096), before, in(2, 512);
            juce::Random r(9);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 4096; ++i) doc.setSample(c, i, r.nextFloat() - 0.5f);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) in.setSample(c, i, (r.nextFloat() - 0.5f) * 0.5f);
            before.makeCopyOf(doc);
            r3wrk::OverdubWriter w; w.prepare();
            for (int i = 0; i < 512; ++i) w.pushPosition(100 + i);
            w.write(doc, in, 512, 0, 0.0f, 1.0f);
            bool same = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 4096; ++i) same = same && doc.getSample(c, i) == before.getSample(c, i);
            check(same, "Level 0, Feedback 100%: the buffer is untouched (bit-exact)");

            w.write(doc, in, 512, 0, 1.0f, 1.0f);
            float err = 0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i)
                err = std::max(err, std::abs(doc.getSample(c, 100 + i) - (before.getSample(c, 100 + i) + in.getSample(c, i))));
            check(err < 1e-6f, "Level 1, Feedback 100%: the new layer is added exactly, both channels");

            doc.makeCopyOf(before);
            in.clear();
            w.write(doc, in, 512, 0, 1.0f, 0.5f);
            err = 0;
            for (int i = 0; i < 512; ++i) err = std::max(err, std::abs(doc.getSample(0, 100 + i) - 0.5f * before.getSample(0, 100 + i)));
            bool untouched = doc.getSample(0, 99) == before.getSample(0, 99) && doc.getSample(0, 612) == before.getSample(0, 612);
            check(err < 1e-6f && untouched, "Feedback 50%: the old audio halves where the pass played, untouched elsewhere");
        }
        {
            juce::AudioBuffer<float> doc(1, 512), in(1, 512);
            doc.clear();
            for (int i = 0; i < 512; ++i) in.setSample(0, i, 0.9f);
            r3wrk::OverdubWriter w; w.prepare();
            for (int pass = 0; pass < 20; ++pass)
            {
                for (int i = 0; i < 512; ++i) w.pushPosition(i);
                w.write(doc, in, 512, 0, 1.0f, 1.0f);
            }
            bool ok = true; float peak = 0;
            for (int i = 0; i < 512; ++i) { ok = ok && std::isfinite(doc.getSample(0, i)); peak = std::max(peak, std::abs(doc.getSample(0, i))); }
            check(ok && peak <= 1.5f, juce::String::formatted("20 stacked full-level passes stay bounded (peak %.3f)", peak));
        }
    }

    // --- Loop reading never leaves the loop (reverse, ping-pong, forward), even at the file's end --
    {
        std::cout << "-- loop reading stays inside the loop (RegionGather.h) --" << std::endl;
        const int docLen = 1000;
        juce::AudioBuffer<float> doc(2, docLen), out(2, 256);
        for (int i = 0; i < docLen; ++i) { doc.setSample(0, i, 0.001f * i); doc.setSample(1, i, -0.001f * i); }
        struct Mode { const char* name; bool pingPong, reverse; int dir; };
        for (const Mode m : { Mode{ "forward loop", false, false, 1 }, Mode{ "ping-pong", true, false, 1 }, Mode{ "reverse loop", false, true, -1 } })
            for (int64_t rs : { (int64_t) 0, (int64_t) 400 })
            {
                const int64_t re = docLen;   // the loop runs to the very end of the audio (e.g. after a Trim)
                int64_t pos = m.reverse ? re - 1 : rs; int dir = m.dir;
                int64_t lo = INT64_MAX, hi = INT64_MIN;
                std::vector<int64_t> trace(256);
                for (int b = 0; b < 40; ++b)
                {
                    out.clear();
                    std::fill(trace.begin(), trace.end(), (int64_t) -1);
                    const int n = r3wrk::gatherRegion(out, 0, 256, 2, doc, pos, dir, rs, re, true, m.pingPong, m.reverse, 20, trace.data());
                    for (int i = 0; i < n; ++i) { lo = std::min(lo, trace[(size_t) i]); hi = std::max(hi, trace[(size_t) i]); }
                }
                check(lo >= rs && hi < re, juce::String(m.name) + juce::String::formatted(" over [%d, %d): reads stay in [%d, %d]",
                                                                                          (int) rs, (int) re, (int) lo, (int) hi));
            }
    }

    // --- Safety net: bad samples can't kill a channel; mono mic detection ---------
    {
        std::cout << "-- safety net (AudioSafety.h) and Dirt self-heal --" << std::endl;
        juce::AudioBuffer<float> b(2, 64); b.clear();
        b.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
        b.setSample(1, 9, std::numeric_limits<float>::infinity());
        b.setSample(0, 5, 0.5f);
        const int fixed = r3wrk::zeroNonFinite(b, 2, 64);
        check(fixed == 2 && b.getSample(0, 3) == 0.0f && b.getSample(1, 9) == 0.0f && b.getSample(0, 5) == 0.5f,
              "zeroNonFinite: NaN/inf become silence, real audio untouched");

        // Dirt fed one NaN at your settings (Drive 36%, Rate 5.2 kHz, 12 bit) keeps playing.
        const double rate01 = std::log(5200.0 / 1000.0) / std::log(sr / 1000.0), bits01 = (12 - 2) / 14.0;
        for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
        {
            r3wrk::DirtStage d; d.prepare(sr); d.snapDrive(0.36);
            auto sig = makeSineBuffer(1, (int) sr * 2, sr, 220.0, 0.4f);
            sig.setSample(0, (int) sr / 2, bad);
            d.process(sig.getWritePointer(0), sig.getNumSamples(), 0.36, rate01, bits01);
            int nonFinite = 0; for (int i = 0; i < sig.getNumSamples(); ++i) nonFinite += std::isfinite(sig.getSample(0, i)) ? 0 : 1;
            check(nonFinite == 0 && sig.getRMSLevel(0, (int) sr, (int) sr) > 0.1f,
                  juce::String("Dirt given one ") + (std::isnan(bad) ? "NaN" : "infinity") + " keeps playing (it used to go silent until restart)");
        }

        juce::AudioBuffer<float> in(2, 128); in.clear();
        for (int i = 0; i < 128; ++i) in.setSample(0, i, 0.3f * (float) std::sin(i * 0.1));
        check(r3wrk::liveInputChannels(in, 2, 128) == 1, "mono mic (left live, right exact silence) -> treated as mono");
        in.setSample(1, 50, 1.0e-6f);
        check(r3wrk::liveInputChannels(in, 2, 128) == 2, "any real signal on the right -> stays stereo");
        in.clear();
        check(r3wrk::liveInputChannels(in, 2, 128) == 2, "both silent -> left as stereo");
    }

    // --- replaceRangeWith used end-to-end (this is what the stretch tool calls) --
    {
        std::cout << "-- replaceRangeWith end-to-end (as used by Apply Stretch/Pitch) --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 4410, sr, 440.0, 0.3f), sr);
        doc.setSelection(1000, 2000); // 1000-sample selection

        juce::AudioBuffer<float> region(1, 1000);
        for (int ch = 0; ch < doc.getBuffer().getNumChannels(); ++ch)
            region.copyFrom(ch, 0, doc.getBuffer(), ch, 1000, 1000);

        auto stretched = TimeStretchEngine::process(region, sr, 2.0, 0.0); // roughly doubles to ~2000 samples
        int64_t before = doc.getNumSamples();
        EditActions::replaceRangeWith(doc, { (int64_t) 1000, (int64_t) 2000 }, stretched, "Time Stretch/Pitch");
        int64_t after = doc.getNumSamples();
        checkNear((double) (after - before), (double) stretched.getNumSamples() - 1000.0, sr * 0.05,
                  "document grew by (stretched length - original selection length)");
    }

    // --- bufferVersion bumps once, so WaveformDisplay can refit the view after a
    // length-growing edit (WaveformDisplay itself isn't reachable from this headless
    // target, so this mirrors its refitViewIfContentChanged() logic against the real
    // AudioDocument/EditActions behaviour instead) --------------------------
    {
        std::cout << "-- bufferVersion / view-refit contract (extreme Stretch) --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(1, 1000, sr, 440.0, 0.3f), sr);
        doc.setSelection(0, 1000);   // stretch the whole (tiny) clip, like an extreme Stretch

        const int versionBefore = doc.getBufferVersion();
        const int64_t totalBefore = doc.getNumSamples();
        // Simulate WaveformDisplay's view having been zoomed all the way out beforehand
        // (viewStart=0, viewEnd=totalBefore), same as after zoomToFit() on file load.
        const int64_t viewStartBefore = 0, viewEndBefore = totalBefore;

        juce::AudioBuffer<float> region(1, 1000);
        region.copyFrom(0, 0, doc.getBuffer(), 0, 0, 1000);
        auto stretched = TimeStretchEngine::process(region, sr, 4.0, 0.0);   // panel's max ratio
        EditActions::replaceRangeWith(doc, { (int64_t) 0, (int64_t) 1000 }, stretched, "Time Stretch/Pitch");

        const int versionAfter = doc.getBufferVersion();
        const int64_t totalAfter = doc.getNumSamples();
        check(versionAfter == versionBefore + 1, "one Stretch/Pitch apply bumps bufferVersion by exactly 1");
        check(totalAfter > totalBefore, "an extreme Stretch actually grew the document");

        // refitViewIfContentChanged()'s own condition, literally: was the view covering
        // [0, totalBefore) just before this version change?
        const bool wasFullView = viewStartBefore <= 0 && viewEndBefore >= totalBefore;
        check(wasFullView, "the pre-edit view (0, totalBefore) reads as \"was showing everything\"");
        const int64_t refitViewEnd = wasFullView ? juce::jmax((int64_t) 1, totalAfter) : viewEndBefore;
        check(refitViewEnd == totalAfter,
              "so the view refits to (0, totalAfter) -- the whole, now-longer document");
    }

    // --- save / load round trip, plus resample-on-load ----------------------
    {
        std::cout << "-- save/load round trip + resample-on-load --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(2, (int) sr, sr, 440.0, 0.6f), sr);

        auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("r3wrk_test_roundtrip.wav");
        tempFile.deleteFile();
        bool saved = doc.saveToFile(tempFile);
        check(saved, "saveToFile wrote a file");
        check(tempFile.getSize() > 44, "saved file has real audio data");

        AudioDocument doc2;
        bool loaded = doc2.loadFromFile(tempFile);
        check(loaded, "loadFromFile read the file back");
        check(doc2.getNumChannels() == 2, "reloaded file kept its channel count");
        checkNear((double) doc2.getNumSamples(), (double) sr, 4.0, "reloaded file kept its sample length");

        AudioDocument doc3;
        bool loadedResampled = doc3.loadFromFile(tempFile, sr * 2.0); // pretend host runs at double rate
        check(loadedResampled, "loadFromFile with resample target succeeded");
        check(std::abs(doc3.getSampleRate() - sr * 2.0) < 0.01, "document sample rate now matches the target rate");
        checkNear((double) doc3.getNumSamples(), (double) sr * 2.0, sr * 0.02,
                  "resampled length roughly doubled to match the new rate");

        tempFile.deleteFile();
    }

    // --- Multi-mode filter: the biquad + it bakes into renderWithPlaybackKnobs ----
    {
        std::cout << "-- multi-mode filter --" << std::endl;

        auto rms = [](const juce::AudioBuffer<float>& b, int ch)
        {
            return (double) b.getRMSLevel(ch, 0, b.getNumSamples());
        };

        // Biquad unit checks: LP well below the tone kills it; HP well above kills it;
        // notch on the tone kills it.
        const int n = (int) sr;
        auto tone8k  = makeSineBuffer(1, n, sr, 8000.0, 0.5f);
        auto tone200 = makeSineBuffer(1, n, sr, 200.0,  0.5f);

        auto runBiquad = [&](juce::AudioBuffer<float> buf, int mode, double fc, double q)
        {
            r3wrk::Biquad bq; bq.setCoeffs(mode, fc, q, sr);
            bq.processBlock(buf.getWritePointer(0), buf.getNumSamples());
            return buf;
        };

        check(rms(runBiquad(tone8k,  r3wrk::filterLP, 300.0, 0.7), 0) < rms(tone8k, 0)  * 0.15,
              "LP at 300 Hz crushes an 8 kHz tone");
        check(rms(runBiquad(tone200, r3wrk::filterHP, 4000.0, 0.7), 0) < rms(tone200, 0) * 0.15,
              "HP at 4 kHz crushes a 200 Hz tone");
        {
            auto tone1k = makeSineBuffer(1, n, sr, 1000.0, 0.5f);
            check(rms(runBiquad(tone1k, r3wrk::filterNotch, 1000.0, 4.0), 0) < rms(tone1k, 0) * 0.3,
                  "notch at 1 kHz cuts a 1 kHz tone");
        }

        // Modelled Monomachine MultiModeFilter: Base 0 + Width 1 is a true passthrough;
        // Base 0 + narrow Width is a low-pass (kills the 8 kHz tone); Base up + Width 1 is a
        // high-pass (kills the 200 Hz tone); HP Q gives a resonant boost at the corner.
        auto runMMF = [&](juce::AudioBuffer<float> buf, double base01, double width01,
                          double hpQ01, double lpQ01,
                          r3wrk::FilterModel model = r3wrk::FilterModel::monomachine)
        {
            r3wrk::MultiModeFilter mmf; mmf.model = model;
            mmf.setParams(base01, width01, hpQ01, lpQ01, sr);
            mmf.processBlock(buf.getWritePointer(0), buf.getNumSamples());
            return buf;
        };
        const auto mnmModel = r3wrk::FilterModel::monomachine;
        check(! r3wrk::filterEngaged(mnmModel, 0.0, 1.0, 0.0, 0.0), "Base 0 + Width 1 reads as not engaged");
        check(  r3wrk::filterEngaged(mnmModel, 0.0, 1.0, 0.5, 0.0), "HP Q alone reads as engaged");
        checkNear(rms(runMMF(makeSineBuffer(1, n, sr, 1000.0, 0.5f), 0.0, 1.0, 0.0, 0.0), 0),
                  rms(makeSineBuffer(1, n, sr, 1000.0, 0.5f), 0), 0.01,
                  "Base 0 + Width 1 passes a 1 kHz tone untouched");
        check(rms(runMMF(tone8k, 0.0, 0.25, 0.0, 0.0), 0) < rms(tone8k, 0) * 0.3,
              "Base 0 + low Width (low-pass) crushes an 8 kHz tone");
        check(rms(runMMF(tone200, 0.6, 1.0, 0.0, 0.0), 0) < rms(tone200, 0) * 0.3,
              "Base up + Width 1 (high-pass) crushes a 200 Hz tone");

        // Resonance: a tone sitting near the low-pass corner comes through louder with LP Q up
        // than with LP Q 0 -- the measured Monomachine filter boosts hard at the corner.
        {
            const double lpFc = r3wrk::mnm::lpCutoffHz(0.0, 0.35);
            auto toneAtFc = makeSineBuffer(1, n, sr, lpFc, 0.3f);
            const double dryQ  = rms(runMMF(juce::AudioBuffer<float>(toneAtFc), 0.0, 0.35, 0.0, 0.0), 0);
            const double resoQ = rms(runMMF(juce::AudioBuffer<float>(toneAtFc), 0.0, 0.35, 0.0, 0.7), 0);
            check(resoQ > dryQ * 1.5, "LP Q boosts a tone at the low-pass corner");
        }

        // Octatrack filter model: same knob layout, different empirical curves.
        {
            using namespace r3wrk;
            check(ot::hpCutoffHz(0.0) == 0.0, "OT: Base 0 => high-pass disengaged");
            check(ot::hpCutoffHz(0.5) < ot::hpCutoffHz(0.9), "OT: HP corner rises with Base");
            check(ot::lpCutoffHz(0.0, 1.0) > ot::lpCutoffHz(0.0, 0.2), "OT: LP corner rises with Width");
            check(ot::resonanceToQ(1.0) > ot::resonanceToQ(0.0), "OT: Q rises with the knob");
            check(! ot::engaged(0.0, 1.0, 0.0), "OT: wide open reads as not engaged");
            check(  ot::engaged(0.4, 1.0, 0.0), "OT: Base up reads as engaged");
            check(! filterEngaged(FilterModel::octatrack, 0.0, 1.0, 0.0, 0.0), "OT: filterEngaged agrees when open");

            auto ot8k = runMMF(makeSineBuffer(1, n, sr, 8000.0, 0.5f),
                               0.0, 0.25, 0.0, 0.0, FilterModel::octatrack);
            check(rms(ot8k, 0) < rms(tone8k, 0) * 0.5, "OT: Base 0 + low Width low-passes an 8 kHz tone");
            auto otPass = runMMF(makeSineBuffer(1, n, sr, 1000.0, 0.5f),
                                 0.0, 1.0, 0.0, 0.0, FilterModel::octatrack);
            checkNear(rms(otPass, 0), rms(makeSineBuffer(1, n, sr, 1000.0, 0.5f), 0), 0.02,
                      "OT: wide open passes a 1 kHz tone untouched");
        }

        // renderWithPlaybackKnobs applies the filter: a low-pass on a 200+8000 Hz mix drops the
        // level (the 8 kHz half is removed); wide open is a passthrough.
        juce::AudioBuffer<float> mix(1, n);
        for (int i = 0; i < n; ++i)
            mix.setSample(0, i, tone200.getSample(0, i) + tone8k.getSample(0, i));

        AudioDocument fdoc;
        setDocumentContent(fdoc, juce::AudioBuffer<float>(mix), sr);

        check(! fdoc.playbackKnobsEngaged(), "filter open -> knobs disengaged");
        auto dry = fdoc.renderWithPlaybackKnobs(fdoc.getBuffer());
        checkNear((double) dry.getNumSamples(), (double) n, 1.0, "filter open -> render is a passthrough (length)");

        fdoc.filterBase.store(0.0);
        fdoc.filterWidth.store(0.25);
        check(fdoc.playbackKnobsEngaged(), "filter on -> playbackKnobsEngaged() true even with stretch centred");
        auto filt = fdoc.renderWithPlaybackKnobs(fdoc.getBuffer());
        check((int64_t) filt.getNumSamples() == n, "filter render keeps the length");
        check(rms(filt, 0) < rms(fdoc.getBuffer(), 0) * 0.85,
              "low-pass at ~500 Hz measurably lowers the 200+8000 Hz mix");

        // Gain knob: 0 dB is a passthrough; -6 dB bakes in as ~half the level.
        AudioDocument gdoc;
        setDocumentContent(gdoc, makeSineBuffer(1, n, sr, 440.0, 0.5f), sr);
        check(! gdoc.playbackKnobsEngaged(), "gain at 0 dB -> knobs disengaged");
        gdoc.playbackGainDb.store(-6.0);
        check(gdoc.playbackKnobsEngaged(), "gain != 0 -> playbackKnobsEngaged() true");
        auto quieter = gdoc.renderWithPlaybackKnobs(gdoc.getBuffer());
        checkNear(rms(quieter, 0), rms(gdoc.getBuffer(), 0) * juce::Decibels::decibelsToGain(-6.0f), 0.005,
                  "-6 dB Gain bakes in as a -6 dB level drop");
    }

    // --- Save bakes the Speed/Pitch/Stretch knobs into the written audio ----
    {
        std::cout << "-- save bakes the playback knobs into the file --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(2, (int) sr, sr, 440.0, 0.6f), sr);
        const int64_t dryLen = doc.getNumSamples();

        check(! doc.playbackKnobsEngaged(), "knobs read as disengaged at identity");
        {
            auto passthrough = doc.renderWithPlaybackKnobs(doc.getBuffer());
            check((int64_t) passthrough.getNumSamples() == dryLen,
                  "renderWithPlaybackKnobs is a no-op copy when the knobs are centred");
        }

        doc.playbackStretch.store(3.0);   // pure time-stretch, pitch preserved
        check(doc.playbackKnobsEngaged(), "knobs read as engaged after turning Stretch up");

        auto stretched = doc.renderWithPlaybackKnobs(doc.getBuffer());
        checkNear((double) stretched.getNumSamples(), (double) dryLen * 3.0, (double) sr * 0.25,
                  "3x Stretch renders ~3x as many samples");

        auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("r3wrk_test_bake.wav");
        tempFile.deleteFile();
        check(doc.saveToFile(tempFile), "saveToFile wrote the baked file");

        AudioDocument reloaded;
        check(reloaded.loadFromFile(tempFile), "reloaded the baked file");
        checkNear((double) reloaded.getNumSamples(), (double) dryLen * 3.0, (double) sr * 0.25,
                  "the saved file is ~3x longer -- the Stretch knob is in the audio, not lost");

        bool nonSilent = false;
        for (int ch = 0; ch < reloaded.getNumChannels() && ! nonSilent; ++ch)
            if (reloaded.getBuffer().getMagnitude(ch, 0, reloaded.getBuffer().getNumSamples()) > 0.05f)
                nonSilent = true;
        check(nonSilent, "the baked file holds real signal");

        tempFile.deleteFile();
    }

    // --- Slice export bakes the knobs AND moves the markers with the stretch ---
    {
        std::cout << "-- slice export: knobs baked, markers scaled with the stretch --" << std::endl;
        AudioDocument doc;
        const int oneSec = (int) sr;
        setDocumentContent(doc, makeSineBuffer(2, oneSec, sr, 330.0, 0.5f), sr);

        doc.addSliceMarker(oneSec / 4);   // 0.25 s
        doc.addSliceMarker(oneSec / 2);   // 0.50 s
        check(doc.getSliceRegions().size() == 3, "2 markers -> 3 slice regions");

        doc.playbackStretch.store(2.0);   // everything twice as long, same pitch

        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("r3wrk_test_slices");
        dir.deleteRecursively();
        const int n = EditActions::sliceToFolder(doc, dir, "s");
        check(n == 3, "wrote 3 slice files");

        auto lenOf = [](const juce::File& f) -> double
        {
            AudioDocument d;
            return d.loadFromFile(f) ? (double) d.getNumSamples() : -1.0;
        };
        const double l0 = lenOf(dir.getChildFile("s 01.wav"));
        const double l1 = lenOf(dir.getChildFile("s 02.wav"));
        const double l2 = lenOf(dir.getChildFile("s 03.wav"));

        // Raw slices are 0.25 s / 0.25 s / 0.50 s; at 2x stretch -> ~0.5 / 0.5 / 1.0 s.
        checkNear(l0, sr * 0.5, sr * 0.05, "slice 1 stretched to ~0.5 s");
        checkNear(l1, sr * 0.5, sr * 0.05, "slice 2 stretched to ~0.5 s");
        checkNear(l2, sr * 1.0, sr * 0.05, "slice 3 stretched to ~1.0 s");
        checkNear(l0 + l1 + l2, sr * 2.0, sr * 0.1, "slices still tile the whole 2x-long render");

        dir.deleteRecursively();
    }

    // --- Octatrack chain export is always 16-bit / 44.1 kHz ----------------
    {
        std::cout << "-- Octatrack chain: forced to 16-bit / 44100 --" << std::endl;
        AudioDocument doc;
        const double srcRate = 48000.0;
        setDocumentContent(doc, makeSineBuffer(2, (int) srcRate, srcRate, 220.0, 0.5f), srcRate);
        doc.addSliceMarker((int64_t) srcRate / 2);

        auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("r3wrk_ot.wav");
        auto ot  = wav.withFileExtension("ot");
        wav.deleteFile(); ot.deleteFile();

        check(EditActions::exportOctatrackChain(doc, wav, 120.0), "exportOctatrackChain wrote the files");
        check(ot.existsAsFile(), "the .ot sits next to the .wav");

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(wav));
        check(rd != nullptr, "the chain .wav reads back");
        if (rd != nullptr)
        {
            checkNear(rd->sampleRate, 44100.0, 1.0, "chain .wav is 44100 Hz even though the doc was 48000");
            check(rd->bitsPerSample == 16, "chain .wav is 16-bit");
            checkNear((double) rd->lengthInSamples, 44100.0, 44100.0 * 0.02, "chain .wav length resampled to ~1 s");
        }
        wav.deleteFile(); ot.deleteFile();
    }

    // --- saveToFile(opts): container format + sample rate + bit depth ------
    {
        std::cout << "-- save options: format / sample rate / bit depth --" << std::endl;
        AudioDocument doc;
        const int oneSec = (int) sr;
        setDocumentContent(doc, makeSineBuffer(2, oneSec, sr, 440.0, 0.5f), sr);

        auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
        auto roundtrip = [&](const juce::File& f, AudioSaveOptions o, double expectRate, double expectLen,
                             const juce::String& what)
        {
            f.deleteFile();
            const bool wrote = doc.saveToFile(f, o);
            check(wrote, what + ": wrote the file");
            AudioDocument rl;
            const bool read = wrote && rl.loadFromFile(f);
            check(read, what + ": read it back");
            if (read)
            {
                checkNear(rl.getSampleRate(), expectRate, 1.0, what + ": sample rate");
                checkNear((double) rl.getNumSamples(), expectLen, sr * 0.05, what + ": length");
            }
            f.deleteFile();
        };

        AudioSaveOptions o;
        o.format = AudioSaveOptions::Format::aiff; o.sampleRate = 0; o.bitDepth = 24;
        roundtrip(tmp.getChildFile("r3wrk_t.aiff"), o, sr, sr, "AIFF 24-bit keep-rate");

        o.format = AudioSaveOptions::Format::flac; o.bitDepth = 32;   // FLAC can't do 32f -> clamps to 24
        roundtrip(tmp.getChildFile("r3wrk_t.flac"), o, sr, sr, "FLAC (32 clamps to 24)");

        o.format = AudioSaveOptions::Format::wav; o.bitDepth = 32; o.sampleRate = 48000;
        roundtrip(tmp.getChildFile("r3wrk_t.wav"), o, 48000.0, sr * (48000.0 / sr), "WAV 32-float @ 48 kHz");

        AudioSaveOptions defOpts;
        check(defOpts.extension() == ".wav", "default options -> .wav");
        AudioSaveOptions flacOpts; flacOpts.format = AudioSaveOptions::Format::flac;
        check(flacOpts.extension() == ".flac", "FLAC options -> .flac");
    }

    // --- channel focus + Match Channels -----------------------------------
    {
        std::cout << "-- channel focus scopes processing; Match Channels balances --" << std::endl;
        const int n = (int) sr;
        juce::AudioBuffer<float> stereo(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = std::sin(2.0 * juce::MathConstants<double>::pi * 300.0 * i / sr);
            stereo.setSample(0, i, 0.20f * s);   // Left quieter
            stereo.setSample(1, i, 0.80f * s);   // Right louder
        }
        AudioDocument doc;
        setDocumentContent(doc, std::move(stereo), sr);

        const float rBefore = doc.getBuffer().getMagnitude(1, 0, n);

        // Focus Left, then Normalize -> only Left moves.
        doc.channelFocus = AudioDocument::ChannelFocus::left;
        EditActions::normalize(doc);
        checkNear(doc.getBuffer().getMagnitude(1, 0, n), rBefore, 1.0e-4, "Normalize with focus=Left left Right untouched");
        checkNear(doc.getBuffer().getMagnitude(0, 0, n), juce::Decibels::decibelsToGain(-0.3f), 0.02f,
                  "Normalize with focus=Left brought Left to ~-0.3 dB");
        doc.undoManager.undo();   // back to 0.20 / 0.80

        // Match Channels (peak): Left comes up to meet Right.
        doc.channelFocus = AudioDocument::ChannelFocus::stereo;
        const auto msg = EditActions::matchChannels(doc, false);
        check(msg.contains("Left"), "Match Channels reported raising Left");
        checkNear(doc.getBuffer().getMagnitude(0, 0, n), doc.getBuffer().getMagnitude(1, 0, n), 0.01f,
                  "after Match Channels the two channel peaks agree");

        // A mono doc has nothing to match.
        AudioDocument monoDoc;
        setDocumentContent(monoDoc, makeSineBuffer(1, n, sr, 300.0, 0.5f), sr);
        check(EditActions::matchChannels(monoDoc, false).isEmpty(), "Match Channels is a no-op on a mono clip");
    }

    // --- Convert to Mono / Convert to Stereo -----------------------------
    {
        std::cout << "-- convert stereo<->mono --" << std::endl;
        const int n = (int) sr;
        juce::AudioBuffer<float> st(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = std::sin(2.0 * juce::MathConstants<double>::pi * 200.0 * i / sr);
            st.setSample(0, i, 0.30f * s);
            st.setSample(1, i, 0.70f * s);
        }
        AudioDocument doc;
        setDocumentContent(doc, std::move(st), sr);

        EditActions::convertToMono(doc);
        check(doc.getNumChannels() == 1, "convertToMono -> 1 channel");
        check(doc.getNumSamples() == (int64_t) n, "convertToMono kept the length");
        checkNear(doc.getBuffer().getMagnitude(0, 0, n), 0.5f, 0.02f, "mono is the average of L+R (~0.5 peak)");

        EditActions::convertToMono(doc);   // already mono
        check(doc.getNumChannels() == 1, "convertToMono on a mono clip is a no-op");

        EditActions::convertToStereo(doc);
        check(doc.getNumChannels() == 2, "convertToStereo -> 2 channels");
        checkNear(doc.getBuffer().getMagnitude(0, 0, n),
                  doc.getBuffer().getMagnitude(1, 0, n), 1.0e-5f, "convertToStereo makes dual mono (channels identical)");

        doc.undoManager.undo();
        check(doc.getNumChannels() == 1, "undo of convertToStereo returns to mono");
        doc.undoManager.undo();
        check(doc.getNumChannels() == 2, "undo of convertToMono returns to stereo");
    }

    // --- LFO-modulated filter (TPT state-variable) stability ----------
    // Verifies r3wrk::ModulatedMultiModeFilter (BiquadFilter.h) offline, BEFORE it's ever wired
    // into live playback -- the direct-form r3wrk::Biquad produced genuinely dangerous, very
    // loud output when continuously modulated fast during live testing (see the LFO design
    // conversation / PROJECT_NOTES); this filter exists specifically to fix that, and the fix
    // needs to be proven here, not by listening.
    {
        std::cout << "-- LFO-modulated filter (ModulatedMultiModeFilter) stability --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) fs;   // 1 second

        // Continuously sweeps Base/Width at `lfoRateHz` while filtering a steady tone, using
        // either the new TPT filter or the existing direct-form one -- the same stress shape
        // (fast modulation + resonance up) that caused trouble live.
        auto runModulated = [&](double lfoRateHz, double amount, double baseCentre, double widthCentre,
                                double hpQ, double lpQ, float inputAmp, bool useTpt)
        {
            r3wrk::ModulatedMultiModeFilter tpt;
            r3wrk::MultiModeFilter          biquad;
            float peak = 0.0f;
            bool hasNonFinite = false;
            double phase = 0.0;
            const double inc = lfoRateHz / fs;

            for (int i = 0; i < numSamples; ++i)
            {
                const float in = inputAmp * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * (double) i / fs);

                const double lfo = std::sin(phase * juce::MathConstants<double>::twoPi);
                phase += inc;
                if (phase >= 1.0) phase -= std::floor(phase);

                const double base  = juce::jlimit(0.0, 1.0, baseCentre  + amount * lfo);
                const double width = juce::jlimit(0.0, 1.0, widthCentre + amount * lfo);

                float out;
                if (useTpt) { tpt.setParams(base, width, hpQ, lpQ, fs);    out = tpt.processSample(in); }
                else        { biquad.setParams(base, width, hpQ, lpQ, fs); out = biquad.processSample(in); }

                if (! std::isfinite(out)) hasNonFinite = true;
                peak = juce::jmax(peak, std::abs(out));
            }
            return std::make_pair(peak, hasNonFinite);
        };

        const double stressRate = 300.0;   // well into "audio rate", the scenario that failed live
        const double amount = 0.4;
        const double base0 = 0.4, width0 = 0.3, hpQ = 0.7, lpQ = 0.7;
        const float inAmp = 0.5f;

        const auto tptResult    = runModulated(stressRate, amount, base0, width0, hpQ, lpQ, inAmp, true);
        const auto biquadResult = runModulated(stressRate, amount, base0, width0, hpQ, lpQ, inAmp, false);

        std::cout << "  TPT peak: "    << tptResult.first
                  << (tptResult.second    ? " (NON-FINITE!)" : "") << std::endl;
        std::cout << "  Biquad peak (for comparison, NOT used live under modulation): " << biquadResult.first
                  << (biquadResult.second ? " (NON-FINITE!)" : "") << std::endl;

        check(! tptResult.second, "TPT filter output stays finite under fast (300Hz) modulation + resonance");
        check(tptResult.first < inAmp * 4.0f, "TPT filter output peak stays bounded (< 4x input) under fast modulation");

        // Sanity check: a fixed-cutoff TPT filter should behave like a real filter (attenuate a
        // well-above-cutoff tone), not just a coincidentally-safe pass-through.
        {
            r3wrk::ModulatedMultiModeFilter fixedTpt;
            fixedTpt.setParams(0.3, 0.05, 0.0, 0.0, fs);
            float peak = 0.0f;
            for (int i = 0; i < (int) fs; ++i)
            {
                const float in = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 8000.0 * (double) i / fs);
                peak = juce::jmax(peak, std::abs(fixedTpt.processSample(in)));
            }
            check(peak < 0.4f, "fixed-cutoff TPT filter attenuates a well-above-cutoff 8kHz tone");
        }
    }

    // --- Erbe-Verb reverb (r3wrk::ErbeVerbReverb) stability ---------------------
    // A feedback delay network is exactly the kind of DSP that can misbehave under an untested
    // parameter combination (see the LFO-modulated filter test above for why this project
    // proves new feedback-based DSP offline before it's ever wired into live playback). Decay
    // pinned at its true max is the case the saturator specifically exists to catch -- that
    // setting is *designed* for near-infinite sustain (the paper's own description), so this
    // block only asserts safety (finite, bounded), never that the tail decays -- a separate
    // block below checks real decay at a moderate Decay setting instead.
    {
        std::cout << "-- Erbe-Verb reverb (ErbeVerbReverb) stability at max Decay --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);   // 3 seconds

        r3wrk::ErbeVerbReverb reverb;
        reverb.prepare(fs);

        float peak = 0.0f;
        bool hasNonFinite = false;

        for (int i = 0; i < numSamples; ++i)
        {
            // Sweep Absorb across its full range over the test; pin Decay at its true max (the
            // saturator's active zone) and Size/Tilt/Mix at fixed, non-trivial values.
            const double absorb01 = (double) i / (double) numSamples;
            reverb.setParams(/*size*/ 0.6, absorb01, /*decay*/ 1.0, /*tilt*/ 0.5,
                             /*mix*/ 1.0, /*predelayMs*/ 20.0);

            const float in = (i == 0) ? 1.0f : 0.0f;   // a single-sample impulse
            float outL, outR;
            reverb.processSample(in, in, outL, outR);

            if (! std::isfinite(outL) || ! std::isfinite(outR)) hasNonFinite = true;
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
        }

        std::cout << "  peak: " << peak << (hasNonFinite ? " (NON-FINITE!)" : "") << std::endl;

        check(! hasNonFinite, "reverb output stays finite under an impulse with Decay pinned at max + a full Absorb sweep");
        check(peak < 4.0f, "reverb output peak stays within the safety clamp (< 4.0) under stress");
    }

    // --- Erbe-Verb reverb: real reverberation + decay at a moderate setting ----
    {
        std::cout << "-- Erbe-Verb reverb: reverberates and decays at moderate Decay --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);   // 3 seconds

        r3wrk::ErbeVerbReverb reverb;
        reverb.prepare(fs);
        // A small room (Size low) so the tail arrives well within the early window regardless
        // of predelay/diffusion transit time, and a moderate Decay (well under the saturator's
        // near-infinite-sustain extreme above) so it's expected to audibly decay within a few
        // seconds, not sustain indefinitely.
        reverb.setParams(/*size*/ 0.1, /*absorb*/ 0.5, /*decay*/ 0.5, /*tilt*/ 0.5,
                         /*mix*/ 1.0, /*predelayMs*/ 10.0);

        float energyEarly = 0.0f;   // 0.2-0.4s after the impulse -- after predelay+network transit
        float energyLate  = 0.0f;   // last 0.2s of the 3s test
        const int earlyStart = (int) (fs * 0.2), earlyEnd = (int) (fs * 0.4);
        const int lateStart  = numSamples - (int) (fs * 0.2);

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = (i == 0) ? 1.0f : 0.0f;
            float outL, outR;
            reverb.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            if (i >= earlyStart && i < earlyEnd) energyEarly += mono * mono;
            if (i >= lateStart) energyLate += mono * mono;
        }

        std::cout << "  early energy (0.2-0.4s): " << energyEarly
                  << ", late energy (last 0.2s of 3s): " << energyLate << std::endl;

        check(energyEarly > 0.0f, "reverb tail has arrived and has energy by 0.2-0.4s after the impulse");
        check(energyLate < energyEarly, "reverb tail has decayed by the end of a 3s window at a moderate Decay setting");
    }

    // --- Erbe-Verb Hadamard feedback matrix ------------------------------------
    // Exercises r3wrk::hadamardFeedback() directly (the same function ErbeVerbReverb::
    // processSample() calls, not a hand-copied duplicate) against the architecture doc's exact
    // row formulas, so a future edit to the matrix math itself would be caught here.
    {
        std::cout << "-- Erbe-Verb Hadamard matrix --" << std::endl;

        const double decayGain = 0.06;
        const float fdn[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        double f[4];
        r3wrk::hadamardFeedback(fdn, decayGain, f);

        // By hand: row1=[1,-1,-1,1]->1-2-3+4=0; row2=[1,1,-1,-1]->1+2-3-4=-4;
        // row3=[1,-1,1,-1]->1-2+3-4=-2; row4=[1,1,1,1]->1+2+3+4=10 -- each * decayGain.
        checkNear(f[0], decayGain * 0.0,  1e-9, "Hadamard row 1 ([1,-1,-1,1]) matches by hand");
        checkNear(f[1], decayGain * -4.0, 1e-9, "Hadamard row 2 ([1,1,-1,-1]) matches by hand");
        checkNear(f[2], decayGain * -2.0, 1e-9, "Hadamard row 3 ([1,-1,1,-1]) matches by hand");
        checkNear(f[3], decayGain * 10.0, 1e-9, "Hadamard row 4 ([1,1,1,1]) matches by hand");
    }

    // --- Plexus matrix interpolation (r3wrk::PlexusMatrix) stability ------------
    // The one genuinely novel piece of DSP in the Plexiphon build (see PlexiphonEngine.h's
    // header comment and ~/Downloads/PLEXIPHON_PLAN.md): naively blending two orthogonal
    // matrices isn't guaranteed to stay energy-preserving at intermediate blend amounts. Proven
    // here, offline, in isolation (no delay lines, no damping, no saturator, no decay-gain
    // scalar -- just the matrix repeatedly bouncing a vector off itself), BEFORE it's wired
    // into the full engine -- the same discipline that caught the Erbe-Verb saturator bug.
    //
    // Measures the matrix's own per-bounce amplitude growth rate at each Plexus setting --
    // PlexiphonEngine's Decay gain cap needs to stay comfortably under 1/growthRate for the
    // combined (matrix * decayGain) loop to be asymptotically stable, so this number is a
    // direct input to that design, not just a pass/fail gate.
    {
        std::cout << "-- Plexus matrix interpolation (PlexusMatrix) stability --" << std::endl;

        bool anyNonFinite = false;
        double worstGrowthPerBounce = 0.0;   // furthest growth/bounce strays from 1.0, either direction
        double worstLogEnergyRatio = 0.0;    // furthest the 500-bounce energy ratio strays from 1.0, in log space

        for (int step = 0; step <= 10; ++step)
        {
            const double amount = (double) step / 10.0;   // Plexus 0..1
            r3wrk::PlexusMatrix m;
            m.setAmount(amount);

            // One line excited (an impulse landing on one delay line), then repeatedly bounced
            // through the matrix alone.
            double v[r3wrk::PlexusMatrix::N] = { 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            double startEnergy = 0.0;
            for (double x : v) startEnergy += x * x;

            constexpr int kBounces = 500;   // far more than a real decay tail needs per line
            for (int b = 0; b < kBounces; ++b)
            {
                double out[r3wrk::PlexusMatrix::N];
                m.apply(v, out);
                for (int i = 0; i < r3wrk::PlexusMatrix::N; ++i)
                {
                    if (! std::isfinite(out[i])) anyNonFinite = true;
                    v[i] = out[i];
                }
            }

            double endEnergy = 0.0;
            for (double x : v) endEnergy += x * x;
            const double energyRatio = endEnergy / juce::jmax(1.0e-15, startEnergy);
            // Energy is amplitude-squared; sqrt of the per-bounce energy ratio is the per-bounce
            // amplitude growth rate.
            const double growthPerBounce = std::isfinite(energyRatio)
                ? std::pow(juce::jmax(1.0e-15, energyRatio), 1.0 / (2.0 * (double) kBounces))
                : std::numeric_limits<double>::infinity();
            worstGrowthPerBounce = juce::jmax(worstGrowthPerBounce, std::abs(growthPerBounce - 1.0));
            worstLogEnergyRatio = juce::jmax(worstLogEnergyRatio,
                                             std::abs(std::log(juce::jmax(1.0e-15, energyRatio))));

            std::cout << "  plexus=" << amount << "  growth/bounce=" << growthPerBounce
                      << "  (energy ratio after " << kBounces << " bounces: " << energyRatio << ")"
                      << std::endl;
        }

        check(! anyNonFinite, "Plexus matrix output stays finite across the full Plexus sweep");
        // Tight: measured (after the spectral-radius fix) within +/-0.3% across the sweep --
        // 1% leaves real headroom while still catching a regression back toward the pre-fix
        // behavior (up to +7.6%/bounce, i.e. worse than 75x this bound).
        check(worstGrowthPerBounce < 0.01,
             "Plexus matrix's own per-bounce growth stays within 1% of neutral across the sweep");
        // Direct, intuitive safety property: after 500 bounces (far more than a real decay tail
        // needs), energy should be neither vanished nor exploded -- bounded within a generous
        // 1000x either direction (measured: well within 10x).
        check(worstLogEnergyRatio < std::log(1000.0),
             "Plexus matrix energy after 500 bounces stays within 1000x of where it started, either direction");
    }

    // --- Plexiphon engine (r3wrk::PlexiphonEngine) stability at max Decay, sweeping Plexus ----
    // Decay pinned at its true max is the case the saturator specifically exists to catch (same
    // reasoning as ErbeVerbReverb's equivalent test) -- that setting is *designed* for near-
    // infinite sustain, so this block only asserts safety (finite, bounded), never that the tail
    // decays. Plexus is swept across its full range over the test, since the matrix topology
    // itself is what's actually novel here.
    {
        std::cout << "-- Plexiphon engine stability at max Decay, sweeping Plexus --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);

        r3wrk::PlexiphonEngine engine;
        engine.prepare(fs);

        float peak = 0.0f;
        bool hasNonFinite = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const double plexus01 = (double) i / (double) numSamples;
            engine.setParams(/*level*/ 0.5, plexus01, /*size*/ 0.4, /*diffuse*/ 0.5,
                             /*decay*/ 1.0, /*color*/ 0.5, /*mix*/ 1.0, /*blockNumSamples*/ 1);

            const float in = (i == 0) ? 1.0f : 0.0f;   // a single-sample impulse
            float outL, outR;
            engine.processSample(in, in, outL, outR);

            if (! std::isfinite(outL) || ! std::isfinite(outR)) hasNonFinite = true;
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
        }

        std::cout << "  peak: " << peak << (hasNonFinite ? " (NON-FINITE!)" : "") << std::endl;

        check(! hasNonFinite, "Plexiphon output stays finite under an impulse with Decay pinned at max + a full Plexus sweep");
        check(peak < 4.0f, "Plexiphon output peak stays within the safety clamp (< 4.0) under stress");
    }

    // --- Plexiphon v2: echo <-> reverb morph, stereo Couple/Skew, cut-only Color ----------
    {
        std::cout << "-- Plexiphon v2 (echo/reverb morph, Couple, Skew, Color) --" << std::endl;
        const double fs = 48000.0;
        struct P { double level = 0.667, plexus = 0.0, size = 0.3, diffuse = 0.0, decay = 0.5, color = 0.5, mix = 1.0, couple = 0.5, skew = 0.5; };
        auto render = [&](const P& pr, std::function<float(int)> inL, std::function<float(int)> inR, int n,
                          std::vector<float>& outL, std::vector<float>& outR)
        {
            r3wrk::PlexiphonEngine e; e.prepare(fs);
            e.setParams(pr.level, pr.plexus, pr.size, pr.diffuse, pr.decay, pr.color, pr.mix, 0, pr.couple, pr.skew);
            outL.assign((size_t) n, 0.0f); outR.assign((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i) e.processSample(inL(i), inR(i), outL[(size_t) i], outR[(size_t) i]);
        };
        auto impulse = [](int i) { return i == 0 ? 1.0f : 0.0f; };
        auto silence = [](int) { return 0.0f; };
        juce::Random rng(11);
        std::vector<float> noiseL((size_t) fs), noiseR((size_t) fs);
        for (size_t i = 0; i < noiseL.size(); ++i) { noiseL[i] = rng.nextFloat() - 0.5f; noiseR[i] = rng.nextFloat() - 0.5f; }
        auto burstL = [&](int i) { return i < (int) (fs * 0.5) ? noiseL[(size_t) i] : 0.0f; };
        auto burstR = [&](int i) { return i < (int) (fs * 0.5) ? noiseR[(size_t) i] : 0.0f; };
        std::vector<float> L, R;

        { // Echo discreteness: one line, clean repeats at a steady period, silence between.
            P pr; pr.couple = 0.0;
            render(pr, impulse, silence, (int) (fs * 2.0), L, R);
            float peakAll = 0; for (float v : L) peakAll = std::max(peakAll, std::abs(v));
            std::vector<int> peaks;
            for (int i = 1; i + 1 < (int) L.size(); ++i)
                if (std::abs(L[(size_t) i]) > 0.05f * peakAll && std::abs(L[(size_t) i]) >= std::abs(L[(size_t) i - 1])
                    && std::abs(L[(size_t) i]) >= std::abs(L[(size_t) i + 1]) && (peaks.empty() || i - peaks.back() > (int) (fs * 0.05)))
                    peaks.push_back(i);
            bool steady = peaks.size() >= 3;
            float between = 0;
            for (size_t k = 1; k < peaks.size() && steady; ++k)
            {
                steady = std::abs((peaks[k] - peaks[k - 1]) - (peaks[1] - peaks[0])) <= (int) (fs * 0.001);
                for (int i = peaks[k - 1] + (int) (fs * 0.03); i < peaks[k] - (int) (fs * 0.03); ++i) between = std::max(between, std::abs(L[(size_t) i]));
            }
            const double sizeMs = (1.0 + 499.0 * pr.size) / 340.0 * 1000.0;
            const double periodMs = peaks.size() >= 2 ? (peaks[1] - peaks[0]) / fs * 1000.0 : 0.0;
            check(steady && between < 0.01f * peakAll && periodMs >= sizeMs && periodMs < sizeMs + 25.0,
                  juce::String::formatted("PLEXUS 0: %d clean repeats every %.1f ms (Size %.1f ms), quiet between (%.4f of peak)",
                                          (int) peaks.size(), periodMs, sizeMs, between / juce::jmax(1e-9f, peakAll)));
        }
        { // Reverb density: far more of the tail is 'busy' at PLEXUS 1 than at 0.
            auto busy = [&](double plexus)
            {
                P pr; pr.plexus = plexus; pr.diffuse = 0.5;
                render(pr, impulse, impulse, (int) (fs * 1.0), L, R);
                int n = 0; for (int i = (int) (fs * 0.5); i < (int) (fs * 1.0); ++i) n += std::abs(L[(size_t) i]) > 1.0e-4f ? 1 : 0;
                return n;
            };
            const int echo = busy(0.0), verb = busy(1.0);
            check(verb > 3 * echo + 1000, juce::String::formatted("PLEXUS 1 is far denser than PLEXUS 0 (%d vs %d busy samples in 0.5-1 s)", verb, echo));
        }
        { // Level stays within +-6 dB across the sweep.
            double lo = 1e9, hi = 0;
            for (double plexus : { 0.0, 0.25, 0.5, 0.75, 1.0 })
            {
                P pr; pr.plexus = plexus; pr.diffuse = 0.5; pr.size = 0.4;
                render(pr, burstL, burstR, (int) (fs * 2.0), L, R);
                double e = 0; for (int i = (int) (fs * 0.1); i < (int) (fs * 2.0); ++i) e += L[(size_t) i] * L[(size_t) i] + R[(size_t) i] * R[(size_t) i];
                const double rms = std::sqrt(e / (2.0 * fs * 1.9));
                lo = std::min(lo, rms); hi = std::max(hi, rms);
            }
            check(hi / lo < 4.0, juce::String::formatted("wet level across PLEXUS 0..1 stays within +-6 dB (span %.1f dB)", 20 * std::log10(hi / lo)));
        }
        { // COUPLE 0 keeps L and R apart; COUPLE 1 interlaces them.
            auto rightRms = [&](double couple)
            {
                P pr; pr.plexus = 0.6; pr.diffuse = 0.5; pr.couple = couple;
                render(pr, burstL, silence, (int) (fs * 1.5), L, R);
                double e = 0; for (float v : R) e += v * v;
                return std::sqrt(e / (double) R.size());
            };
            const double iso = rightRms(0.0), full = rightRms(1.0);
            check(iso < 1.0e-7 && full > 0.005, juce::String::formatted("COUPLE 0: input on L never reaches R (rms %.1e); COUPLE 1 does (%.4f)", iso, full));
        }
        { // COLOR direction: anticlockwise, each repeat loses highs; clockwise, each loses lows.
            auto hfShare = [&](double color, int echoIdx)
            {
                P pr; pr.color = color; pr.decay = 0.8; pr.couple = 0.0;
                std::vector<float> nz((size_t) (fs * 0.01)); juce::Random r2(3);
                for (auto& v : nz) v = r2.nextFloat() - 0.5f;
                render(pr, [&](int i) { return i < (int) nz.size() ? nz[(size_t) i] : 0.0f; }, silence, (int) (fs * 2.5), L, R);
                const double sizeS = (1.0 + 499.0 * pr.size) / 340.0 + 0.0172;
                const int c = (int) std::round(sizeS * fs * echoIdx);
                // Tilt = high-band energy (first difference) over low-band energy (one-pole
                // low-pass at ~300 Hz) -- compares the two ends directly, in dB.
                double low = 0, diff = 0, lp = 0;
                const double a = std::exp(-2.0 * juce::MathConstants<double>::pi * 300.0 / fs);
                for (int i = c - (int) (fs * 0.004); i < c + (int) (fs * 0.02); ++i)
                {
                    lp = a * lp + (1.0 - a) * L[(size_t) i];
                    low += lp * lp;
                    const float d = L[(size_t) i] - L[(size_t) i - 1]; diff += d * d;
                }
                return 10.0 * std::log10(diff / juce::jmax(1e-15, low));
            };
            const double dark1 = hfShare(0.0, 1), dark4 = hfShare(0.0, 4), bright1 = hfShare(1.0, 1), bright4 = hfShare(1.0, 4);
            const double flat1 = hfShare(0.5, 1), flat4 = hfShare(0.5, 4);
            check(std::abs(flat4 - flat1) < 0.5,
                  juce::String::formatted("COLOR centred: repeats keep their tone (tilt %+.2f dB over 3 repeats)", flat4 - flat1));
            check(dark4 - dark1 < -2.0 && bright4 - bright1 > 2.0,
                  juce::String::formatted("COLOR: anticlockwise darkens each repeat (tilt %+.1f dB over 3 repeats), clockwise brightens it (%+.1f dB)", dark4 - dark1, bright4 - bright1));
        }
        { // COLOR never boosts: every shelf setting it can reach stays at or under 0 dB everywhere.
            double worst = 0;
            for (int g = 0; g <= 24; ++g)
                for (int hi = 0; hi < 2; ++hi)
                {
                    r3wrk::Biquad b;
                    if (hi) r3wrk::setHighShelfCoeffs(b, 800.0, 0.5, -0.1 * g, fs); else r3wrk::setLowShelfCoeffs(b, 800.0, 0.5, -0.1 * g, fs);
                    for (int k = 1; k < 400; ++k)
                    {
                        const double w = juce::MathConstants<double>::pi * k / 400.0;
                        const std::complex<double> z = std::polar(1.0, -w), z2 = z * z;
                        const double mag = std::abs((b.b0 + b.b1 * z + b.b2 * z2) / (1.0 + b.a1 * z + b.a2 * z2));
                        worst = std::max(worst, mag);
                    }
                }
            check(worst <= 1.0 + 1e-9, juce::String::formatted("COLOR's cut-only shelves never exceed 0 dB at any frequency (max gain %.9f)", worst));
        }
        { // Stability: PLEXUS x COUPLE x SKEW x COLOR, DECAY at max, noise then silence, knobs moving per block.
            const double fsS = 16000.0;
            bool finite = true; float peak = 0;
            for (double pl : { 0.0, 0.25, 0.5, 0.75, 1.0 })
            for (double cp : { 0.0, 0.25, 0.5, 0.75, 1.0 })
            for (double sk : { 0.0, 0.5, 1.0 })
            for (double co : { 0.0, 0.5, 1.0 })
            {
                r3wrk::PlexiphonEngine e; e.prepare(fsS);
                juce::Random r3(5);
                for (int i = 0; i < (int) (fsS * 2.0); ++i)
                {
                    if (i % 256 == 0)
                    {
                        const double wob = 0.1 * std::sin(i * 0.001);
                        e.setParams(1.0, juce::jlimit(0.0, 1.0, pl + wob), 0.3, 0.7, 1.0, co, 1.0, 256, cp, sk);
                    }
                    const float x = i < (int) fsS ? (r3.nextFloat() - 0.5f) : 0.0f;
                    float oL, oR; e.processSample(x, -x, oL, oR);
                    finite = finite && std::isfinite(oL) && std::isfinite(oR);
                    peak = std::max(peak, std::max(std::abs(oL), std::abs(oR)));
                }
            }
            check(finite && peak < 4.0f, juce::String::formatted("225 PLEXUS/COUPLE/SKEW/COLOR combos at max DECAY: finite, within the clamp (peak %.2f)", peak));
        }
        { // SIZE glides: sweeping it in echo mode bends pitch smoothly, no jumps.
            r3wrk::PlexiphonEngine e; e.prepare(fs);
            float worst = 0, prevOut = 0; bool first = true;
            for (int i = 0; i < (int) (fs * 1.5); ++i)
            {
                if (i % 512 == 0)
                    e.setParams(0.667, 0.0, 0.2 + 0.3 * juce::jmin(1.0, i / fs), 0.0, 0.3, 0.5, 1.0, 512, 0.0, 0.5);
                const float x = 0.3f * (float) std::sin(2 * juce::MathConstants<double>::pi * 220.0 * i / fs);
                float oL, oR; e.processSample(x, x, oL, oR);
                if (! first) worst = std::max(worst, std::abs(oL - prevOut));
                prevOut = oL; first = false;
            }
            const float slope = (float) (0.3 * 2 * juce::MathConstants<double>::pi * 220.0 / fs) * 3.0f;   // loop builds up to ~1/(1-0.28)
            check(worst < slope * 2.0f, juce::String::formatted("sweeping SIZE in echo mode glides, no jumps (worst step %.4f, smooth limit %.4f)", worst, slope * 2.0f));
        }
    }

    // --- Plexiphon engine: real reverberation/echo + decay at a moderate setting -------------
    {
        std::cout << "-- Plexiphon engine: reverberates and decays at moderate Decay --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);

        r3wrk::PlexiphonEngine engine;
        engine.prepare(fs);
        engine.setParams(/*level*/ 0.5, /*plexus*/ 0.5, /*size*/ 0.1, /*diffuse*/ 0.5,
                         /*decay*/ 0.5, /*color*/ 0.5, /*mix*/ 1.0, /*blockNumSamples*/ numSamples);

        float energyEarly = 0.0f;   // 0.2-0.4s after the impulse
        float energyLate  = 0.0f;   // last 0.2s of the 3s test
        const int earlyStart = (int) (fs * 0.2), earlyEnd = (int) (fs * 0.4);
        const int lateStart  = numSamples - (int) (fs * 0.2);

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = (i == 0) ? 1.0f : 0.0f;
            float outL, outR;
            engine.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            if (i >= earlyStart && i < earlyEnd) energyEarly += mono * mono;
            if (i >= lateStart) energyLate += mono * mono;
        }

        std::cout << "  early energy (0.2-0.4s): " << energyEarly
                  << ", late energy (last 0.2s of 3s): " << energyLate << std::endl;

        check(energyEarly > 0.0f, "Plexiphon tail/echo has arrived and has energy by 0.2-0.4s after the impulse");
        check(energyLate < energyEarly, "Plexiphon tail has decayed by the end of a 3s window at a moderate Decay setting");
    }

    // --- Mimeophon Zone table (r3wrk::mimeoZoneRangeMs) -------------------------
    // The doc this was built from explicitly asks for the derived Zone-range formula to be
    // checked against the manual's own published table before anything else depends on it.
    // The manual's own numbers are already rounded to ~4 significant figures, so a small
    // relative tolerance (not exact-to-the-digit) is the right bar.
    {
        std::cout << "-- Mimeophon Zone table --" << std::endl;

        struct Row { int zone; double minMs, maxMs; };
        const Row table[] = {
            { 0, 1.33,   20.4   },
            { 1, 20.4,   81.6   },
            { 2, 81.6,   326.5  },
            { 3, 163.3,  653.1  },
            { 4, 326.5,  1306.0 },
            { 5, 653.1,  2612.0 },
            { 6, 1306.0, 5225.0 },
            { 7, 2612.0, 41796.0 },
        };

        bool allWithinTolerance = true;
        for (const auto& row : table)
        {
            const auto got = r3wrk::mimeoZoneRangeMs(row.zone);
            const double minErr = std::abs(got.minMs - row.minMs) / row.minMs;
            const double maxErr = std::abs(got.maxMs - row.maxMs) / row.maxMs;
            std::cout << "  zone " << row.zone << ": got [" << got.minMs << ", " << got.maxMs
                      << "], table [" << row.minMs << ", " << row.maxMs << "]"
                      << ", rel err [" << minErr << ", " << maxErr << "]" << std::endl;
            if (minErr > 0.01 || maxErr > 0.01)
                allWithinTolerance = false;
        }

        check(allWithinTolerance, "mimeoZoneRangeMs() reproduces all 8 published Zone rows within 1%");
    }

    // --- Mimeophon engine stability at max Repeats, sweeping Zone ---------------
    // Repeats pinned at its true max is the case the saturator specifically exists to catch --
    // designed for self-oscillation, so this block only asserts safety (finite, bounded), never
    // that the tail decays (same reasoning as the other two engines' equivalent tests).
    {
        std::cout << "-- Mimeophon engine stability at max Repeats, sweeping Zone --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);

        r3wrk::MimeophonEngine engine;
        engine.prepare(fs);

        float peak = 0.0f;
        bool hasNonFinite = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const double zone01 = (double) i / (double) numSamples;
            engine.setParams(zone01, /*rate*/ 0.5, /*repeats*/ 1.0, /*color*/ 0.5,
                             /*halo*/ 0.5, /*mix*/ 1.0);

            const float in = (i == 0) ? 1.0f : 0.0f;   // a single-sample impulse
            float outL, outR;
            engine.processSample(in, in, outL, outR);

            if (! std::isfinite(outL) || ! std::isfinite(outR)) hasNonFinite = true;
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
        }

        std::cout << "  peak: " << peak << (hasNonFinite ? " (NON-FINITE!)" : "") << std::endl;

        check(! hasNonFinite, "Mimeophon output stays finite under an impulse with Repeats pinned at max + a full Zone sweep");
        check(peak < 4.0f, "Mimeophon output peak stays within the safety clamp (< 4.0) under stress");
    }

    // --- Mimeophon engine: real echo timing + decay at a moderate setting -------
    // Unlike Erbe-Verb/Plexiphon (reverbs, no single "the echo"), a delay's correctness can be
    // checked more directly: confirm a distinct echo actually arrives close to the Zone/Rate-
    // implied delay time, and that Repeats well under max actually decays rather than sustains.
    {
        std::cout << "-- Mimeophon engine: echo arrives on time and decays at moderate Repeats --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);

        r3wrk::MimeophonEngine engine;
        engine.prepare(fs);
        // Zone 3 (163.3-653.1ms), Rate at the zone's geometric centre -> expected delay ~ that
        // zone's midpoint; Repeats moderate (well under the self-oscillation cap).
        engine.setParams(/*zone*/ 3.0 / 7.0, /*rate*/ 0.5, /*repeats*/ 0.5, /*color*/ 0.5,
                         /*halo*/ 0.3, /*mix*/ 1.0);
        const auto zoneRange = r3wrk::mimeoZoneRangeMs(3);
        const double expectedMs = std::sqrt(zoneRange.minMs * zoneRange.maxMs);   // geometric centre, matches Rate's own log mapping

        int firstEchoSample = -1;
        float peakBeforeExpected = 0.0f, peakNearExpected = 0.0f;
        float energyEarly = 0.0f, energyLate = 0.0f;
        const int expectedSample = (int) (expectedMs * 0.001 * fs);
        const int windowStart = juce::jmax(0, expectedSample - (int) (fs * 0.05));
        const int windowEnd   = expectedSample + (int) (fs * 0.05);
        const int lateStart = numSamples - (int) (fs * 0.2);

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = (i == 0) ? 1.0f : 0.0f;
            float outL, outR;
            engine.processSample(in, in, outL, outR);
            const float mono = 0.5f * (outL + outR);
            const float absMono = std::abs(mono);

            if (i < windowStart) peakBeforeExpected = juce::jmax(peakBeforeExpected, absMono);
            if (i >= windowStart && i < windowEnd)
            {
                peakNearExpected = juce::jmax(peakNearExpected, absMono);
                if (firstEchoSample < 0 && absMono > 0.05f) firstEchoSample = i;
            }
            if (i >= windowStart && i < windowStart + (int) (fs * 0.1)) energyEarly += mono * mono;
            if (i >= lateStart) energyLate += mono * mono;
        }

        std::cout << "  expected echo at " << expectedMs << " ms (sample " << expectedSample << ")"
                  << ", first echo detected at sample " << firstEchoSample
                  << ", peak before window: " << peakBeforeExpected
                  << ", peak in window: " << peakNearExpected << std::endl;

        check(firstEchoSample >= 0, "a distinct echo arrives within 50ms of the Zone/Rate-implied delay time");
        check(peakBeforeExpected < 0.05f, "nothing echoes back before the expected delay time (no premature leakage)");
        check(energyLate < energyEarly, "Mimeophon tail decays by the end of a 3s window at a moderate Repeats setting");
    }

    // --- Mimeophon Ping-Pong: stability at max Repeats, sweeping Zone -----------
    // Same stress shape as the plain max-Repeats sweep above, but with Ping-Pong engaged --
    // the cross-channel feedback routing is new code (MimeoChannel::processRead/commitWrite +
    // MimeophonEngine::processSample's routing), so it gets its own stability gate before being
    // trusted, same discipline as every other new subsystem in this codebase.
    {
        std::cout << "-- Mimeophon Ping-Pong: stability at max Repeats, sweeping Zone --" << std::endl;
        const double fs = 48000.0;
        const int numSamples = (int) (fs * 3.0);

        r3wrk::MimeophonEngine engine;
        engine.prepare(fs);

        float peak = 0.0f;
        bool hasNonFinite = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const double zone01 = (double) i / (double) numSamples;
            engine.setParams(zone01, /*rate*/ 0.5, /*repeats*/ 1.0, /*color*/ 0.5,
                             /*halo*/ 0.5, /*mix*/ 1.0, /*skew*/ 0.5, /*pingPong*/ true);

            const float in = (i == 0) ? 1.0f : 0.0f;   // a single-sample impulse
            float outL, outR;
            engine.processSample(in, in, outL, outR);

            if (! std::isfinite(outL) || ! std::isfinite(outR)) hasNonFinite = true;
            peak = juce::jmax(peak, std::abs(outL), std::abs(outR));
        }

        std::cout << "  peak: " << peak << (hasNonFinite ? " (NON-FINITE!)" : "") << std::endl;

        check(! hasNonFinite, "Mimeophon Ping-Pong output stays finite under an impulse with Repeats pinned at max + a full Zone sweep");
        check(peak < 4.0f, "Mimeophon Ping-Pong output peak stays within the safety clamp (< 4.0) under stress");
    }

    // --- Mimeophon Ping-Pong: repeats actually bounce across channels -----------
    // The correctness check unique to Ping-Pong: feed an impulse into L only and confirm the
    // repeats alternate sides -- echo 1 (the direct delay tap) stays on L, echo 2 (the first
    // repeat generated FROM feedback) crosses to R, echo 3 bounces back to L. Halo off and a
    // short Zone keep the three echo windows cleanly separated for this check.
    {
        std::cout << "-- Mimeophon Ping-Pong: repeats bounce L->R->L --" << std::endl;
        const double fs = 48000.0;

        r3wrk::MimeophonEngine engine;
        engine.prepare(fs);
        engine.setParams(/*zone*/ 1.0 / 7.0, /*rate*/ 0.5, /*repeats*/ 0.5, /*color*/ 0.5,
                         /*halo*/ 0.0, /*mix*/ 1.0, /*skew*/ 0.5, /*pingPong*/ true);

        const auto zoneRange = r3wrk::mimeoZoneRangeMs(1);
        const double delayMs = std::sqrt(zoneRange.minMs * zoneRange.maxMs);
        const double delaySamples = delayMs * 0.001 * fs;
        const int numSamples = (int) (delaySamples * 4.0);

        std::vector<float> outLbuf((size_t) numSamples), outRbuf((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = (i == 0) ? 1.0f : 0.0f;
            float outL, outR;
            engine.processSample(inL, 0.0f, outL, outR);
            outLbuf[(size_t) i] = outL;
            outRbuf[(size_t) i] = outR;
        }

        auto peakInWindow = [&](const std::vector<float>& buf, double centreSample)
        {
            const int halfWin = juce::jmax(4, (int) (delaySamples * 0.35));
            const int lo = juce::jmax(0, (int) centreSample - halfWin);
            const int hi = juce::jmin(numSamples - 1, (int) centreSample + halfWin);
            float peak = 0.0f;
            for (int i = lo; i <= hi; ++i) peak = juce::jmax(peak, std::abs(buf[(size_t) i]));
            return peak;
        };

        const float echo1L = peakInWindow(outLbuf, delaySamples * 1.0);
        const float echo1R = peakInWindow(outRbuf, delaySamples * 1.0);
        const float echo2L = peakInWindow(outLbuf, delaySamples * 2.0);
        const float echo2R = peakInWindow(outRbuf, delaySamples * 2.0);
        const float echo3L = peakInWindow(outLbuf, delaySamples * 3.0);
        const float echo3R = peakInWindow(outRbuf, delaySamples * 3.0);

        std::cout << "  echo1 L=" << echo1L << " R=" << echo1R
                  << ", echo2 L=" << echo2L << " R=" << echo2R
                  << ", echo3 L=" << echo3L << " R=" << echo3R << std::endl;

        check(echo1L > echo1R * 3.0f, "Ping-Pong echo 1 (the direct delay tap) stays on the input's own channel (L)");
        check(echo2R > echo2L * 3.0f, "Ping-Pong echo 2 (the first feedback-born repeat) bounces to the other channel (R)");
        check(echo3L > echo3R * 3.0f, "Ping-Pong echo 3 bounces back to the original channel (L)");
    }

    // --- Mimeophon Ping-Pong: still bounces with identical L/R input (mono source) ----------
    // The bug the user actually hit: R3WRK plays back loaded audio, and most of it is mono
    // duplicated into both channels. The test above alone can't catch that -- it only ever fed
    // the impulse into L, so it kept passing while Ping-Pong was inaudible on real (mono)
    // material, since crossing two identical feedback signals used to be a no-op. Feed the
    // SAME impulse into both channels here -- the routing fix (mono dry into the left line
    // only) must still produce a clear alternating bounce.
    {
        std::cout << "-- Mimeophon Ping-Pong: still bounces with identical L/R input (mono source) --" << std::endl;
        const double fs = 48000.0;

        r3wrk::MimeophonEngine engine;
        engine.prepare(fs);
        engine.setParams(/*zone*/ 1.0 / 7.0, /*rate*/ 0.5, /*repeats*/ 0.5, /*color*/ 0.5,
                         /*halo*/ 0.0, /*mix*/ 1.0, /*skew*/ 0.5, /*pingPong*/ true);

        const auto zoneRange = r3wrk::mimeoZoneRangeMs(1);
        const double delayMs = std::sqrt(zoneRange.minMs * zoneRange.maxMs);
        const double delaySamples = delayMs * 0.001 * fs;
        const int numSamples = (int) (delaySamples * 4.0);

        std::vector<float> outLbuf((size_t) numSamples), outRbuf((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = (i == 0) ? 1.0f : 0.0f;   // identical on both channels -- the mono case
            float outL, outR;
            engine.processSample(in, in, outL, outR);
            outLbuf[(size_t) i] = outL;
            outRbuf[(size_t) i] = outR;
        }

        auto peakInWindow = [&](const std::vector<float>& buf, double centreSample)
        {
            const int halfWin = juce::jmax(4, (int) (delaySamples * 0.35));
            const int lo = juce::jmax(0, (int) centreSample - halfWin);
            const int hi = juce::jmin(numSamples - 1, (int) centreSample + halfWin);
            float peak = 0.0f;
            for (int i = lo; i <= hi; ++i) peak = juce::jmax(peak, std::abs(buf[(size_t) i]));
            return peak;
        };

        const float echo1L = peakInWindow(outLbuf, delaySamples * 1.0);
        const float echo1R = peakInWindow(outRbuf, delaySamples * 1.0);
        const float echo2L = peakInWindow(outLbuf, delaySamples * 2.0);
        const float echo2R = peakInWindow(outRbuf, delaySamples * 2.0);
        const float echo3L = peakInWindow(outLbuf, delaySamples * 3.0);
        const float echo3R = peakInWindow(outRbuf, delaySamples * 3.0);

        std::cout << "  echo1 L=" << echo1L << " R=" << echo1R
                  << ", echo2 L=" << echo2L << " R=" << echo2R
                  << ", echo3 L=" << echo3L << " R=" << echo3R << std::endl;

        check(echo1L > echo1R * 3.0f, "mono-source echo 1 stays on the left (mono dry only enters the left line)");
        check(echo2R > echo2L * 3.0f, "mono-source echo 2 bounces to the right");
        check(echo3L > echo3R * 3.0f, "mono-source echo 3 bounces back to the left");
    }

    {
        std::cout << "\n[DragScan] no clicks while dragging a loop edge" << std::endl;
        // Drives the real dragscan::renderBlock with processBlock's own region slew (replicated
        // here: 8/s distance-proportional, capped at 1.5x, throttled for short loops), over a
        // constant-1.0 document -- so the output IS the gain envelope, and any sample-to-sample
        // step in it is a click regardless of material. Before the per-sample-edge fix these
        // scenarios measured steps of 0.25-1.0 (up to ~12 clicks/s on short loops).
        const double sr = 44100.0;
        const int block = 512;
        juce::AudioBuffer<float> ones(1, (int) (sr * 12));
        for (int i = 0; i < ones.getNumSamples(); ++i) ones.setSample(0, i, 1.0f);

        struct Scenario { const char* name; std::function<double(double)> start, end; double pos0; };
        const Scenario scenarios[] = {
            { "drag End forward (1 s loop)", [](double) { return 0.0; },            [&](double t) { return sr + sr * t; },  0.0 },
            { "drag End backward",           [](double) { return 0.0; },            [&](double t) { return 3 * sr - 0.6 * sr * t; }, 0.0 },
            { "drag Start forward",          [&](double t) { return 0.2 * sr * t; }, [&](double) { return 2 * sr; },         0.0 },
            { "move whole window backward",  [&](double t) { return std::max(0.0, 3 * sr - sr * t); },
                                             [&](double t) { return 4 * sr - sr * t; }, 3 * sr },
            { "150 ms loop, drag End forward", [](double) { return 0.0; },          [&](double t) { return 0.15 * sr + 0.5 * sr * t; }, 0.0 },
            { "30 ms loop, End back and forth", [](double) { return 0.0; },
                                             [&](double t) { return 0.03 * sr + 0.05 * sr * std::abs(std::sin(5 * t)); }, 0.0 },
        };
        juce::Random jitter(42);
        for (const auto& sc : scenarios)
        {
            for (int withJitter = 0; withJitter < 2; ++withJitter)
            {
                double ds = std::floor(sc.start(0)), de = std::floor(sc.end(0)), pos = sc.pos0;
                dragscan::Relocation rel;
                float prev = -1.0f, maxStep = 0.0f;
                juce::AudioBuffer<float> out(1, block);
                for (int b = 0; b < (int) (3.0 * sr / block); ++b)
                {
                    const double t = b * block / sr;
                    double rs = std::floor(sc.start(t)), re = std::floor(sc.end(t));
                    if (withJitter) { re += jitter.nextInt({ -300, 301 }); if (rs > 0) rs = std::max(0.0, rs + jitter.nextInt({ -300, 301 })); }
                    const double targetLen = std::max(1.0, re - rs);
                    const double maxSlew = dragscan::maxWindowSpeed(sr, 1.0, targetLen);
                    const double dt = block / sr;
                    const double s0 = ds, e0 = de;
                    ds += juce::jlimit(-maxSlew, maxSlew, (rs - ds) * 8.0) * dt;
                    de += juce::jlimit(-maxSlew, maxSlew, (re - de) * 8.0) * dt;
                    out.clear();
                    dragscan::renderBlock(out, 1, block, ones, pos, rel, s0, e0, ds, de, true, 0.010 * sr, sr);
                    for (int i = 0; i < block; ++i)
                    {
                        const float y = out.getSample(0, i);
                        if (prev >= 0.0f) maxStep = std::max(maxStep, std::abs(y - prev));
                        prev = y;
                    }
                }
                // A 10 ms raised-cosine fade moves at most pi/2 / 441 ~ 0.0036 per sample; allow
                // a few times that for a window racing toward the playhead.
                check(maxStep < 0.02f, juce::String(sc.name) + (withJitter ? " + mouse jitter" : "")
                                          + juce::String::formatted(": largest gain step %.4f", maxStep));
            }
        }
        check(dragscan::edgeFadeGain(-0.3, 1000, 441) < 0.001 && dragscan::edgeFadeGain(999.4, 1000, 441) < 0.001,
              "edge fade is ~0 just outside either edge (crossing an edge is silent)");
    }

    {
        std::cout << "\n[DragScan] a dragged loop never rises in pitch" << std::endl;
        // Doc value == sample index, loop fades off: every output step IS the read speed. The
        // renderer used to catch up to a window that outran it by reading up to 2x fast (steps of
        // ~2 for long runs -- the pitch rise); it now always reads at 1x and relocates instead.
        // Anything longer than a crossfade (10 ms) at a step clearly above 1 is a speed-up.
        const double sr = 44100.0;
        const int block = 512;
        juce::AudioBuffer<float> ramp(1, (int) (sr * 12));
        for (int n = 0; n < ramp.getNumSamples(); ++n) ramp.setSample(0, n, (float) n);
        struct Sc { const char* name; std::function<double(double)> s, e; };
        const Sc scs[] = {
            { "Start dragged forward fast", [&](double t) { return std::floor(juce::jmin(1.0, t / 0.5) * 4 * sr); }, [&](double) { return 6 * sr; } },
            { "loop moved forward 4 s",     [&](double t) { return std::floor(juce::jmin(1.0, t) * 4 * sr); }, [&](double t) { return std::floor(juce::jmin(1.0, t) * 4 * sr + 0.5 * sr); } },
            { "short loop moved forward",   [&](double t) { return std::floor(juce::jmin(1.0, t) * 2 * sr); }, [&](double t) { return std::floor(juce::jmin(1.0, t) * 2 * sr + 0.08 * sr); } },
            { "loop moved backward 4 s",    [&](double t) { return std::floor(4 * sr - juce::jmin(1.0, t) * 4 * sr); }, [&](double t) { return std::floor(4.5 * sr - juce::jmin(1.0, t) * 4 * sr); } },
        };
        for (const auto& sc : scs)
        {
            double ds = sc.s(0), de = sc.e(0), pos = ds;
            dragscan::Relocation rel;
            juce::AudioBuffer<float> out(1, block);
            float prev = -1.0f;
            int run = 0, longestFastRun = 0, oneX = 0, total = 0;
            for (int b = 0; b < (int) (2.0 * sr / block); ++b)
            {
                const double t = b * block / sr;
                const double rs = sc.s(t), re = sc.e(t);
                const double maxSlew = dragscan::maxWindowSpeed(sr, 1.0, (re - rs));
                const double s0 = ds, e0 = de;
                ds += juce::jlimit(-maxSlew, maxSlew, (rs - ds) * 8.0) * block / sr;
                de += juce::jlimit(-maxSlew, maxSlew, (re - de) * 8.0) * block / sr;
                out.clear();
                dragscan::renderBlock(out, 1, block, ramp, pos, rel, s0, e0, ds, de, true, 0.0, sr);
                for (int i = 0; i < block; ++i)
                {
                    const float y = out.getSample(0, i);
                    if (prev >= 0.0f)
                    {
                        const float step = y - prev;
                        ++total;
                        if (std::abs(step - 1.0f) < 1.0e-3f) ++oneX;
                        run = (step > 1.2f && step < 3.0f) ? run + 1 : 0;
                        longestFastRun = std::max(longestFastRun, run);
                    }
                    prev = y;
                }
            }
            check(longestFastRun < (int) (0.010 * sr),
                  juce::String(sc.name) + juce::String::formatted(": %.1f%% of samples at exactly 1x, longest sped-up run %d samples",
                                                                  100.0 * oneX / juce::jmax(1, total), longestFastRun));
        }
    }

    {
        std::cout << "\n[DragScan] a dragged loop keeps playing while it slides" << std::endl;
        // Reading at 1x only works if the window can't outrun the playhead: with the old 1.5x
        // window cap a fast forward Start drag pinned the playhead at the loop seam (91% of the
        // slide silent). Sine material, 10 ms loop fade; a 10 ms stretch peaking under 0.1 is
        // counted silent.
        const double sr = 44100.0; const int block = 512;
        juce::AudioBuffer<float> doc(1, (int) (sr * 12));
        for (int n = 0; n < doc.getNumSamples(); ++n) doc.setSample(0, n, (float) std::sin(2 * juce::MathConstants<double>::pi * 110 * n / sr));
        struct Sc { const char* name; std::function<double(double)> s, e; };
        const Sc scs[] = {
            { "Start dragged forward fast", [&](double t) { return std::floor(juce::jmin(1.0, t / 0.5) * 4 * sr); }, [&](double) { return 6 * sr; } },
            { "loop moved forward 4 s",     [&](double t) { return std::floor(juce::jmin(1.0, t) * 4 * sr); }, [&](double t) { return std::floor(juce::jmin(1.0, t) * 4 * sr + 0.5 * sr); } },
            { "loop moved backward 4 s",    [&](double t) { return std::floor(4 * sr - juce::jmin(1.0, t) * 4 * sr); }, [&](double t) { return std::floor(4.5 * sr - juce::jmin(1.0, t) * 4 * sr); } },
        };
        for (const auto& sc : scs)
        {
            double ds = sc.s(0), de = sc.e(0), pos = ds; dragscan::Relocation rel;
            juce::AudioBuffer<float> out(1, block);
            int quiet = 0, total = 0, wn = 0; float win = 0;
            for (int b = 0; b < (int) (3.0 * sr / block); ++b)
            {
                const double t = b * block / sr; const double rs = sc.s(t), re = sc.e(t);
                const double maxSlew = dragscan::maxWindowSpeed(sr, 1.0, re - rs);
                const double s0 = ds, e0 = de;
                ds += juce::jlimit(-maxSlew, maxSlew, (rs - ds) * 8.0) * block / sr;
                de += juce::jlimit(-maxSlew, maxSlew, (re - de) * 8.0) * block / sr;
                out.clear();
                dragscan::renderBlock(out, 1, block, doc, pos, rel, s0, e0, ds, de, true, 441, sr);
                for (int i = 0; i < block; ++i)
                {
                    win = std::max(win, std::abs(out.getSample(0, i)));
                    if (++wn == 441) { ++total; quiet += win < 0.1f ? 1 : 0; win = 0; wn = 0; }
                }
            }
            check(quiet * 100 <= total * 3, juce::String(sc.name) + juce::String::formatted(": silent %d%% of the slide", 100 * quiet / juce::jmax(1, total)));
        }
    }

    {
        std::cout << "\n[DragScan] no click when a dragged loop is let go" << std::endl;
        // Random drag-then-release gestures (loop 50 ms-1 s, dropped 0-6 s away, released 0-0.3 s
        // after the mouse stops) over two-sine material. The drag half is the real renderer;
        // after release, the ordinary path is mirrored here (region snaps to the target, a stray
        // playhead jumps to the loop start, raised-cosine loop fade, 8 ms sin^2 ramp -- see
        // processBlock), with and without the real renderReleaseTail as the crossfade's old side.
        const double sr = 44100.0;
        const int block = 512, xfade = 441, declick = (int) (0.008 * sr);
        juce::AudioBuffer<float> doc(1, (int) (sr * 9));
        for (int n = 0; n < doc.getNumSamples(); ++n)
            doc.setSample(0, n, (float) (0.8 * std::sin(2 * juce::MathConstants<double>::pi * 97 * n / sr)
                                         + 0.2 * std::sin(2 * juce::MathConstants<double>::pi * 1234 * n / sr)));
        const float legitSlope = (float) (0.8 * 2 * juce::MathConstants<double>::pi * 97 / sr
                                          + 0.2 * 2 * juce::MathConstants<double>::pi * 1234 / sr);
        auto oldLoopFade = [](int64_t rp, int64_t len, int fadeLen)
        {
            double x;
            if (rp < fadeLen)                  x = (double) rp / fadeLen;
            else if (rp >= len - fadeLen)      x = (double) (len - 1 - rp) / fadeLen;
            else                               return 1.0;
            const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * juce::jlimit(0.0, 1.0, x));
            return sn * sn;
        };

        juce::Random rng(3);
        int clicksWithout = 0, clicksWith = 0;
        float worstWith = 0.0f;
        const int trials = 300;
        for (int t = 0; t < trials; ++t)
        {
            const int64_t loopLen = (int64_t) ((0.05 + 0.95 * rng.nextDouble()) * sr);
            const double start0 = std::floor(rng.nextDouble() * 2 * sr);
            const double dest = std::floor(rng.nextDouble() * 6 * sr);
            const double dragSecs = 0.3 + 0.9 * rng.nextDouble();
            const double holds[] = { 0.0, 0.0, 0.05, 0.1, 0.3 };
            const double holdSecs = holds[rng.nextInt(5)];

            double ds = start0, de = start0 + (double) loopLen, pos = start0;
            dragscan::Relocation rel;
            juce::AudioBuffer<float> out(1, block);
            float last = 0.0f;
            for (int b = 0; b < (int) ((dragSecs + holdSecs) * sr / block); ++b)
            {
                const double frac = juce::jmin(1.0, b * block / sr / dragSecs);
                const double rs = std::floor(start0 + (dest - start0) * frac), re = rs + (double) loopLen;
                const double maxSlew = dragscan::maxWindowSpeed(sr, 1.0, (double) loopLen);
                const double s0 = ds, e0 = de;
                ds += juce::jlimit(-maxSlew, maxSlew, (rs - ds) * 8.0) * block / sr;
                de += juce::jlimit(-maxSlew, maxSlew, (re - de) * 8.0) * block / sr;
                out.clear();
                dragscan::renderBlock(out, 1, block, doc, pos, rel, s0, e0, ds, de, true, xfade, sr);
                last = out.getSample(0, block - 1);
            }

            juce::AudioBuffer<float> tail;
            dragscan::renderReleaseTail(tail, 1, declick, doc, pos, rel, ds, juce::jmax(ds + 1.0, de), true, xfade, sr);

            for (int withTail = 0; withTail < 2; ++withTail)
            {
                const int64_t rs = (int64_t) dest, re = rs + loopLen;
                int64_t p = std::llround(pos);
                const bool snapped = p < rs || p >= re;
                if (snapped) p = rs;
                float prev = last, worst = 0.0f;
                for (int i = 0; i < block; ++i)
                {
                    float y = doc.getSample(0, (int) p) * (float) oldLoopFade(p - rs, loopLen, (int) juce::jmin<int64_t>(xfade, loopLen / 2));
                    const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * juce::jmin(1.0, (double) i / declick));
                    const float g = (float) (sn * sn);
                    if (withTail)     y = y * g + (i < declick ? tail.getSample(0, i) * (1.0f - g) : 0.0f);
                    else if (snapped) y = y * g;   // before: ramp-in only, the old audio just stops
                    worst = std::max(worst, std::abs(y - prev));
                    prev = y;
                    if (++p >= re) p = rs;
                }
                const bool click = worst > legitSlope * 3.0f;
                if (withTail) { clicksWith += click ? 1 : 0; worstWith = std::max(worstWith, worst); }
                else          clicksWithout += click ? 1 : 0;
            }
        }
        check(clicksWithout > trials / 5,
              juce::String::formatted("without the release tail these gestures do click (%d of %d) -- the test can see it", clicksWithout, trials));
        check(clicksWith == 0,
              juce::String::formatted("with renderReleaseTail: %d of %d releases click (worst step %.3f, 1x material max %.3f)",
                                      clicksWith, trials, worstWith, legitSlope));
    }

    {
        std::cout << "\n[DragScan] stretched: dragging and releasing a loop on a time-stretched file" << std::endl;
        // The real stretcher the plugin runs (LofiStretch: Paulstretch) at stretch 2 and 0.5. Mirrors
        // processBlock's stretched drag: the region slews once per block; "before" snaps the
        // playhead into the window whenever it falls outside (+ the output-side 8 ms ramp), "after"
        // feeds RubberBand from dragscan::renderBlock with edges placed by input fed / expected
        // (as renderDragScanStretched does). Then a release mid-slide: the region snaps to its
        // target, a stray playhead jumps to the loop start, with and without the input-side
        // renderReleaseTail blend. Material: two sines -- any step past ~3x the fastest legit
        // slope (at the 2x catch-up read) is a click.
        const double sr = 44100.0;
        const int block = 512, xfade = 441, declick = (int) (0.008 * sr);
        const double twoPi = 2 * juce::MathConstants<double>::pi;
        juce::AudioBuffer<float> doc(1, (int) (sr * 12));
        for (int n = 0; n < doc.getNumSamples(); ++n)
            doc.setSample(0, n, (float) (0.6 * std::sin(twoPi * 110 * n / sr) + 0.2 * std::sin(twoPi * 440 * n / sr)));
        const float clickStep = (float) ((0.6 * 110 + 0.2 * 440) * twoPi / sr) * 2.0f * 3.0f;
        auto loopFade = [](int64_t rp, int64_t len, int f) {
            if (f <= 0) return 1.0; double x;
            if (rp < f) x = (double) rp / f; else if (rp >= len - f) x = (double) (len - 1 - rp) / f; else return 1.0;
            const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * juce::jlimit(0.0, 1.0, x)); return sn * sn; };

        struct Sc { const char* name; std::function<double(double)> s, e; };
        const Sc scs[] = {
            { "Start dragged forward 3 s", [&](double t) { return std::floor(juce::jmin(1.0, t) * 3 * sr); }, [&](double) { return 6 * sr; } },
            { "loop moved forward 3 s",    [&](double t) { return std::floor(juce::jmin(1.0, t) * 3 * sr); }, [&](double t) { return std::floor(juce::jmin(1.0, t) * 3 * sr + 0.5 * sr); } },
            { "loop moved backward 3 s",   [&](double t) { return std::floor(3 * sr - juce::jmin(1.0, t) * 3 * sr); }, [&](double t) { return std::floor(3.5 * sr - juce::jmin(1.0, t) * 3 * sr); } },
        };
        int beforeClicks = 0, releaseBeforeClicks = 0;
        for (double timeRatio : { 2.0, 0.5 })
        for (const auto& sc : scs)
        for (int after = 0; after < 2; ++after)
        {
            r3wrk::LofiStretch rb;
            rb.prepare(sr, 1);
            rb.setParams(1.0, timeRatio, 0.0);
            rb.reset();
            juce::AudioBuffer<float> in(1, 16384), outB(1, block), tail;
            double ds = sc.s(0), de = sc.e(0), dpos = ds;
            dragscan::Relocation rel;
            int64_t pos = (int64_t) ds;
            int outRamp = 0, tailRem = 0;
            bool released = false;
            std::vector<float> y;
            size_t releaseAt = 0;
            const double releaseT = 0.6;   // mid-slide: the window hasn't arrived yet
            for (int b = 0; b < (int) (2.0 * sr / block); ++b)
            {
                const double t = b * block / sr;
                const double rs = sc.s(t), re = sc.e(t);
                double s0 = ds, e0 = de;
                int64_t R0, R1;
                if (t < releaseT)
                {
                    // "before" is the old code as it was: a 1.5x window cap, not scaled for stretch.
                    const double maxSlew = after ? dragscan::maxWindowSpeed(sr, timeRatio, (re - rs))
                                                 : juce::jlimit(sr * 0.1, sr * 1.5, (re - rs) * 8.0 * 0.35);
                    ds += juce::jlimit(-maxSlew, maxSlew, (rs - ds) * 8.0) * block / sr;
                    de += juce::jlimit(-maxSlew, maxSlew, (re - de) * 8.0) * block / sr;
                    R0 = std::llround(ds); R1 = std::max(R0 + 1, (int64_t) std::llround(de));
                    if (! after && (pos < R0 || pos >= R1)) { pos = R0; outRamp = declick; }
                }
                else
                {
                    // Released: the selection stops where the mouse let go, and the region snaps to it.
                    R0 = (int64_t) sc.s(releaseT); R1 = (int64_t) sc.e(releaseT);
                    if (! released)
                    {
                        released = true; releaseAt = y.size();
                        if (after)
                        {
                            dragscan::renderReleaseTail(tail, 1, declick, doc, dpos, rel, ds, std::max(ds + 1.0, de), true, xfade, sr);
                            tailRem = declick;
                            pos = std::llround(dpos);
                        }
                        if (pos < R0 || pos >= R1) { pos = R0; if (! after) outRamp = declick; }
                    }
                }
                const int64_t L = R1 - R0;
                const int fl = (int) std::min<int64_t>(xfade, L / 2);
                const bool dragRender = after && ! released;
                const double expectIn = block / timeRatio; double fed = 0;
                int produced = 0, guard = 4000;
                while (produced < block && --guard > 0)
                {
                    const int avail = (int) rb.available();
                    if (avail > 0) { const int n = std::min(avail, block - produced); float* op[1] = { outB.getWritePointer(0, produced) }; rb.retrieve(op, n); produced += n; continue; }
                    int req = (int) rb.getSamplesRequired(); req = juce::jlimit(1, 16384, req > 0 ? req : 256);
                    in.clear();
                    if (dragRender)
                    {
                        const double f0 = juce::jmin(1.0, fed / expectIn), f1 = juce::jmin(1.0, (fed + req) / expectIn);
                        dragscan::renderBlock(in, 1, req, doc, dpos, rel, s0 + (ds - s0) * f0, e0 + (de - e0) * f0,
                                              s0 + (ds - s0) * f1, e0 + (de - e0) * f1, true, xfade, sr);
                    }
                    else
                    {
                        for (int i = 0; i < req; ++i)
                        {
                            if (pos >= R1) pos = R0;
                            float v = doc.getSample(0, (int) pos) * (float) loopFade(pos - R0, L, fl);
                            if (tailRem > 0)
                            {
                                const double x = 1.0 - (double) tailRem / declick; const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * x);
                                v = v * (float) (sn * sn) + tail.getSample(0, declick - tailRem) * (float) (1.0 - sn * sn); --tailRem;
                            }
                            in.setSample(0, i, v); ++pos;
                        }
                    }
                    fed += req;
                    const float* ip[1] = { in.getReadPointer(0) };
                    rb.process(ip, req, false);
                }
                for (int i = 0; i < block; ++i)
                {
                    float v = outB.getSample(0, i);
                    if (outRamp > 0) { const double x = 1.0 - (double) outRamp / declick; const double sn = std::sin(0.5 * juce::MathConstants<double>::pi * x); v *= (float) (sn * sn); --outRamp; }
                    y.push_back(v);
                }
            }
            int dragClicks = 0, relClicks = 0; float worst = 0;
            for (size_t n = (size_t) (0.3 * sr); n < y.size(); ++n)
            {
                const float d = std::abs(y[n] - y[n - 1]); worst = std::max(worst, d);
                if (d > clickStep) ++(n < releaseAt ? dragClicks : relClicks);
            }
            const juce::String label = juce::String::formatted("ratio %.1f, ", timeRatio) + sc.name;
            if (! after) { beforeClicks += dragClicks; releaseBeforeClicks += relClicks; continue; }
            check(dragClicks == 0 && relClicks == 0,
                  label + juce::String::formatted(": %d clicks while dragging, %d after release (worst step %.3f)", dragClicks, relClicks, worst));
        }
        check(beforeClicks > 10, juce::String::formatted("the old snap-into-window path does click while dragging (%d) -- the test can see it", beforeClicks));
        std::cout << "  (old path, releases: " << releaseBeforeClicks << " clicks)" << std::endl;
    }

    {
        std::cout << "\n[Theme] shared text format (R3WRK <-> Sieve)" << std::endl;
        // Pure Palette parsing only -- no ThemeManager, so the real settings file and the
        // shared themes folder are never touched.
        Palette p;
        p.windowBg = juce::Colour(0xff8797ac);
        p.gridLine = juce::Colour(0x80000000);
        p.shadedPanel = true;
        p.edgeShadeAlpha = 0.3f;
        check(Palette::fromString(p.toString()) == p, "toString -> fromString round-trips");
        check(Palette::fromString(p.toString()).toString() == p.toString(), "text round-trips byte for byte");

        Palette q;
        juce::String name;
        check(Palette::fromClipboardText(p.toClipboardText("Grey Blue"), q, &name) && q == p && name == "Grey Blue",
              "clipboard text round-trips with its name");
        check(Palette::fromClipboardText(p.toString(), q) && q == p, "bare toString() pastes too");
        check(! Palette::fromClipboardText("hello there", q), "non-theme text is rejected");
        check(Palette::fromString("futureKey:ff00ff00;accent:ff112233").accent == juce::Colour(0xff112233)
                  && Palette::fromString("futureKey:ff00ff00") == Palette(),
              "unknown keys are ignored, missing keys keep Midnight defaults");
    }

    std::cout << "===========================================" << std::endl;
    if (failures == 0)
        std::cout << "ALL CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " CHECK(S) FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
