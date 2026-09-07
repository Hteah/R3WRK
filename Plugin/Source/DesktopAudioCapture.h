#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>

/**
    Captures the Mac's system audio output with ScreenCaptureKit (macOS 13+), for the
    Standalone app's "Record Desktop" button. ScreenCaptureKit only vends audio as part of a
    screen-capture stream, so this takes a 2x2 "video" feed it never looks at and keeps the
    audio; the current process is excluded so R3WRK never records its own playback. The first
    start() triggers the system Screen Recording permission prompt.

    All three callbacks fire on a background dispatch queue -- the owner marshals to whatever
    thread it needs (PluginProcessor uses MessageManager::callAsync for onStarted/onError and
    accumulates onSamples straight into a lock-guarded buffer). Not for VST3/AU -- a plugin
    doesn't grab system audio; the host provides audio.
*/
class DesktopAudioCapture
{
public:
    DesktopAudioCapture();
    ~DesktopAudioCapture();

    /// True when ScreenCaptureKit audio capture is available (macOS 13 or later).
    static bool isSupported();

    /// Begins the async start sequence (permission check -> shareable content -> stream).
    ///   onSamples(deinterleaved float [channels][frames], channels, frames, sampleRate)
    ///   onStarted() -- once the stream is actually running
    ///   onError(message) -- no permission / no display / bad format / stream stopped
    void start (std::function<void (const float* const*, int, int, double)> onSamples,
                std::function<void ()>                                      onStarted,
                std::function<void (juce::String)>                          onError);

    /// Stops the stream and drains the sample queue; safe to call when not running.
    void stop();

    bool isRunning() const;

    // Public only so the ObjC++ SCStreamOutput delegate in DesktopAudioCapture.mm (a
    // file-scope @interface, outside this class) can name it -- still an opaque pimpl,
    // defined entirely in the .mm.
    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DesktopAudioCapture)
};
