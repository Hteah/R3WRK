// Cocoa/CoreMedia first, JUCE second -- same ordering JUCE's own native .mm files use, so
// Carbon's global `Component` typedef (pulled in transitively) doesn't collide with
// juce::Component. Compiled with -fobjc-arc (see Plugin/CMakeLists.txt) -- ObjC objects held
// in the C++ Impl below are strong refs managed by its ctor/dtor.
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "DesktopAudioCapture.h"
#include <atomic>
#include <vector>

#if JUCE_MAC

// ---------------------------------------------------------------------------------------------

struct DesktopAudioCapture::Impl
{
    SCStream* stream = nil;                 // strong (ARC)
    id streamOutput = nil;                  // strong (ARC) -- the R3WRKSCKOutput delegate
    dispatch_queue_t sampleQueue = nil;     // strong (ARC)

    std::function<void (const float* const*, int, int, double)> onSamples;
    std::function<void ()>                                      onStarted;
    std::function<void (juce::String)>                          onError;

    std::atomic<bool> running { false };

    // Reused scratch for the interleaved -> deinterleaved case.
    std::vector<std::vector<float>> deint;
    std::vector<const float*>       ptrs;

    void handleAudio (CMSampleBufferRef sb);
};

// ---------------------------------------------------------------------------------------------

API_AVAILABLE(macos(13.0))
@interface R3WRKSCKOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) DesktopAudioCapture::Impl* owner;
@end

@implementation R3WRKSCKOutput

- (void) stream: (SCStream*) stream
        didOutputSampleBuffer: (CMSampleBufferRef) sampleBuffer
        ofType: (SCStreamOutputType) type
{
    juce::ignoreUnused (stream);
    if (type != SCStreamOutputTypeAudio)
        return;
    auto* o = self.owner;
    if (o == nullptr || ! o->running.load (std::memory_order_relaxed))
        return;
    if (! CMSampleBufferIsValid (sampleBuffer) || CMSampleBufferGetNumSamples (sampleBuffer) <= 0)
        return;
    o->handleAudio (sampleBuffer);
}

- (void) stream: (SCStream*) stream didStopWithError: (NSError*) error
{
    juce::ignoreUnused (stream);
    auto* o = self.owner;
    if (o == nullptr)
        return;
    o->running.store (false, std::memory_order_relaxed);
    if (o->onError)
        o->onError (error != nil ? juce::String::fromUTF8 (error.localizedDescription.UTF8String)
                                 : juce::String ("Desktop capture stopped."));
}

@end

// ---------------------------------------------------------------------------------------------

void DesktopAudioCapture::Impl::handleAudio (CMSampleBufferRef sb)
{
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription (sb);
    if (fd == nullptr)
        return;
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription (fd);
    if (asbd == nullptr || (asbd->mFormatFlags & kAudioFormatFlagIsFloat) == 0)
        return;

    const int    channels = (int) asbd->mChannelsPerFrame;
    const double sr        = asbd->mSampleRate > 0 ? asbd->mSampleRate : 48000.0;
    const int    frames    = (int) CMSampleBufferGetNumSamples (sb);
    if (channels <= 0 || frames <= 0)
        return;

    // Two-call pattern: ask the needed AudioBufferList size, then fill it (deinterleaved
    // float => one buffer per channel).
    size_t ablSize = 0;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (sb, &ablSize, nullptr, 0,
                                                                nullptr, nullptr, 0, nullptr) != noErr
        || ablSize == 0)
        return;

    std::vector<uint8_t> storage (ablSize);
    auto* abl = reinterpret_cast<AudioBufferList*> (storage.data());
    CMBlockBufferRef block = nullptr;
    const OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
        sb, nullptr, abl, ablSize, nullptr, nullptr,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (st != noErr || abl->mNumberBuffers == 0)
    {
        if (block != nullptr) CFRelease (block);
        return;
    }

    if ((int) ptrs.size() != channels)
        ptrs.assign ((size_t) channels, nullptr);

    if ((int) abl->mNumberBuffers == channels)
    {
        for (int c = 0; c < channels; ++c)
            ptrs[(size_t) c] = reinterpret_cast<const float*> (abl->mBuffers[c].mData);
    }
    else if (abl->mNumberBuffers == 1
             && (int) abl->mBuffers[0].mNumberChannels == channels)
    {
        const auto* inter = reinterpret_cast<const float*> (abl->mBuffers[0].mData);
        if ((int) deint.size() != channels)
            deint.assign ((size_t) channels, {});
        for (int c = 0; c < channels; ++c)
        {
            deint[(size_t) c].resize ((size_t) frames);
            for (int f = 0; f < frames; ++f)
                deint[(size_t) c][(size_t) f] = inter[f * channels + c];
            ptrs[(size_t) c] = deint[(size_t) c].data();
        }
    }
    else
    {
        if (block != nullptr) CFRelease (block);
        return;
    }

    if (onSamples)
        onSamples (ptrs.data(), channels, frames, sr);

    if (block != nullptr)
        CFRelease (block);
}

// ---------------------------------------------------------------------------------------------

DesktopAudioCapture::DesktopAudioCapture()
    : impl (std::make_unique<Impl>())
{
    impl->sampleQueue = dispatch_queue_create ("com.heathaudio.r3wrk.desktopcapture", DISPATCH_QUEUE_SERIAL);
}

DesktopAudioCapture::~DesktopAudioCapture()
{
    stop();
    if (impl->streamOutput != nil)
        ((R3WRKSCKOutput*) impl->streamOutput).owner = nullptr;
    impl->streamOutput = nil;
    impl->sampleQueue = nil;
}

bool DesktopAudioCapture::isSupported()
{
    if (@available (macOS 13.0, *))
        return NSClassFromString (@"SCStream") != nil;
    return false;
}

bool DesktopAudioCapture::isRunning() const
{
    return impl->running.load (std::memory_order_relaxed);
}

void DesktopAudioCapture::start (std::function<void (const float* const*, int, int, double)> onSamples,
                                 std::function<void ()>                                      onStarted,
                                 std::function<void (juce::String)>                          onError)
{
    impl->onSamples = std::move (onSamples);
    impl->onStarted = std::move (onStarted);
    impl->onError   = std::move (onError);

    if (! isSupported())
    {
        if (impl->onError) impl->onError ("Desktop-audio capture needs macOS 13 or later.");
        return;
    }

    if (@available (macOS 13.0, *))
    {
        Impl* raw = impl.get();
        [SCShareableContent getShareableContentExcludingDesktopWindows: NO
                                                  onScreenWindowsOnly: NO
                                                    completionHandler: ^(SCShareableContent* content, NSError* err)
        {
            if (err != nil || content == nil || content.displays.count == 0)
            {
                if (raw->onError)
                    raw->onError (err != nil ? juce::String::fromUTF8 (err.localizedDescription.UTF8String)
                                             : juce::String ("Screen Recording permission is needed "
                                                             "(System Settings > Privacy & Security > Screen Recording)."));
                return;
            }

            SCDisplay* display = content.displays.firstObject;
            NSString*  myBundleID = NSBundle.mainBundle.bundleIdentifier;
            NSMutableArray<SCRunningApplication*>* mine = [NSMutableArray array];
            for (SCRunningApplication* app in content.applications)
                if (myBundleID != nil && [app.bundleIdentifier isEqualToString: myBundleID])
                    [mine addObject: app];

            SCContentFilter* filter =
                [[SCContentFilter alloc] initWithDisplay: display
                                  excludingApplications: mine
                                       exceptingWindows: @[]];

            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.capturesAudio                = YES;
            cfg.excludesCurrentProcessAudio  = YES;
            cfg.sampleRate                   = 48000;
            cfg.channelCount                 = 2;
            cfg.width                        = 2;
            cfg.height                       = 2;
            cfg.minimumFrameInterval         = CMTimeMake (1, 6);
            cfg.queueDepth                   = 6;

            R3WRKSCKOutput* out = [[R3WRKSCKOutput alloc] init];
            out.owner = raw;
            raw->streamOutput = out;

            SCStream* s = [[SCStream alloc] initWithFilter: filter configuration: cfg delegate: out];

            NSError* addErr = nil;
            [s addStreamOutput: out type: SCStreamOutputTypeAudio  sampleHandlerQueue: raw->sampleQueue error: &addErr];
            [s addStreamOutput: out type: SCStreamOutputTypeScreen sampleHandlerQueue: raw->sampleQueue error: &addErr];

            [s startCaptureWithCompletionHandler: ^(NSError* startErr)
            {
                if (startErr != nil)
                {
                    if (raw->onError)
                        raw->onError (juce::String::fromUTF8 (startErr.localizedDescription.UTF8String));
                    return;
                }
                raw->running.store (true, std::memory_order_relaxed);
                if (raw->onStarted)
                    raw->onStarted();
            }];

            raw->stream = s;
        }];
    }
}

void DesktopAudioCapture::stop()
{
    impl->running.store (false, std::memory_order_relaxed);

    if (@available (macOS 13.0, *))
    {
        SCStream* s = impl->stream;
        impl->stream = nil;
        if (s != nil)
            [s stopCaptureWithCompletionHandler: ^(NSError*) { }];
    }

    // Drain anything already dispatched to the sample queue so no callback fires after this
    // returns and the owner can finalise straight away.
    if (impl->sampleQueue != nil)
        dispatch_sync (impl->sampleQueue, ^{ });
}

#else // ! JUCE_MAC -- capture unavailable; keep the type compilable everywhere.

struct DesktopAudioCapture::Impl {};
DesktopAudioCapture::DesktopAudioCapture() : impl (std::make_unique<Impl>()) {}
DesktopAudioCapture::~DesktopAudioCapture() = default;
bool DesktopAudioCapture::isSupported() { return false; }
bool DesktopAudioCapture::isRunning() const { return false; }
void DesktopAudioCapture::start (std::function<void (const float* const*, int, int, double)>,
                                 std::function<void ()>,
                                 std::function<void (juce::String)> onError)
{
    if (onError) onError ("Desktop-audio capture is macOS-only.");
}
void DesktopAudioCapture::stop() {}

#endif
