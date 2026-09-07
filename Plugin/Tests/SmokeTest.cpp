// Headless correctness smoke test for the core editing engine (no GUI, no audio device).
// Exercises AudioDocument + EditActions + TimeStretchEngine directly.

#include <JuceHeader.h>
#include "../Source/AudioDocument.h"
#include "../Source/EditActions.h"
#include "../Source/TimeStretchEngine.h"

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
        std::cout << "-- time-stretch / pitch-shift (RubberBand) --" << std::endl;
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

    std::cout << "===========================================" << std::endl;
    if (failures == 0)
        std::cout << "ALL CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " CHECK(S) FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
