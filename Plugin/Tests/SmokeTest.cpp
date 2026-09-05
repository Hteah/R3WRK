// Headless correctness smoke test for the core editing engine (no GUI, no audio device).
// Exercises AudioDocument + EditActions + TimeStretchEngine directly.

#include <JuceHeader.h>
#include "../Source/AudioDocument.h"
#include "../Source/EditActions.h"
#include "../Source/TimeStretchEngine.h"
#include "../Source/WaveformStretchPreview.h"

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

    // --- WaveformStretchPreview: the real background stretch behind the live waveform
    // preview (WaveformDisplay draws this instead of a rescaled view of the original once
    // it's ready) -- exercises the actual debounce -> background thread -> RubberBand ->
    // delivery pipeline end to end, not just the math -------------------------
    {
        std::cout << "-- WaveformStretchPreview (background real-stretch for the waveform) --" << std::endl;
        AudioDocument doc;
        setDocumentContent(doc, makeSineBuffer(2, (int) sr, sr, 440.0, 0.3f), sr);   // 1 second, stereo

        WaveformStretchPreview preview(doc);

        bool sawChange = false;
        for (int i = 0; i < 5; ++i)
            sawChange |= preview.update();
        check(! sawChange, "identity knobs never produce a preview");
        check(! preview.hasPreview(), "no preview available at identity");

        // Turn the (equivalent of the) Stretch knob to 3x and wait for the debounced
        // background job to settle, finish, and bin its output into a peak cache.
        doc.playbackStretch = 3.0;
        bool gotResult = false;
        auto deadline = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(10.0);
        while (juce::Time::getCurrentTime() < deadline)
        {
            if (preview.update() && preview.hasPreview())
            {
                gotResult = true;
                break;
            }
            juce::Thread::sleep(20);
        }
        check(gotResult, "a non-identity Stretch eventually produces a real preview");
        checkNear((double) preview.getProcessedLength(), sr * 3.0, sr * 0.1,
                  "the processed length is roughly 3x, matching the real offline stretch");
        check(preview.getNumChannels() == 2, "preview kept the channel count");

        float mn = 0.0f, mx = 0.0f;
        preview.getPeakRange(0, 0, preview.getProcessedLength(), mn, mx);
        check(mx > 0.05f && mn < -0.05f, "the peak cache holds real (non-silent) signal, not just zeros");

        // Back to identity: the preview should clear.
        doc.playbackStretch = 1.0;
        bool cleared = false;
        deadline = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(2.0);
        while (juce::Time::getCurrentTime() < deadline)
        {
            if (preview.update())
            {
                cleared = ! preview.hasPreview();
                break;
            }
            juce::Thread::sleep(20);
        }
        check(cleared, "returning to identity clears the preview");
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

    std::cout << "===========================================" << std::endl;
    if (failures == 0)
        std::cout << "ALL CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " CHECK(S) FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
