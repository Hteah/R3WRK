// Cocoa first, JUCE second -- Carbon's Components.h (pulled in transitively by Cocoa.h) declares
// a plain global `Component` typedef that collides with juce::Component once JUCE's own headers
// bring that name into scope, the same ordering JUCE's own native .mm files always use.
#import <Cocoa/Cocoa.h>
#include "StandaloneWindowShape.h"

#if JUCE_MAC

// Weak no-op so the VST3/AU targets link -- the real one is defined in the patched
// juce_StandaloneFilterWindow.h, which only compiles into the Standalone target and there wins
// the link (strong beats weak). Called from EditorToolbar's "Audio Settings…" menu item.
extern "C" __attribute__((weak)) void r3wrkShowAudioSettings() {}

void r3wrkApplyRoundedWindowCorners (void* windowPtr, float cornerRadiusPx)
{
    auto* window = static_cast<juce::Component*> (windowPtr);
    if (window == nullptr)
        return;

    auto* peer = window->getPeer();
    if (peer == nullptr)
        return;

    // On macOS, ComponentPeer::getNativeHandle() is the backing NSView*.
    NSView* view = (NSView*) peer->getNativeHandle();
    if (view == nil)
        return;

    NSWindow* nsWindow = view.window;
    if (nsWindow == nil)
        return;

    // The window frame is native now (StandaloneFilterWindow's ctor calls
    // setUsingNativeTitleBar(true)), so the close/minimise controls are the standard macOS
    // traffic lights on the LEFT. Hide the title-bar strip itself and let the app's own dark
    // UI run the full height of the window under the buttons -- the buttons just float over
    // the top-left. PluginEditor keeps that strip clear of its own controls in the Standalone
    // build (a small top inset, standalone-only).
    nsWindow.titlebarAppearsTransparent = YES;
    nsWindow.titleVisibility = NSWindowTitleHidden;
    nsWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
    nsWindow.movableByWindowBackground = YES;   // drag the window by its background, no strip to grab

    // Let the desktop show through the four corners the layer mask below crops away, instead
    // of a mismatched solid square peeking out from behind the rounded content.
    nsWindow.opaque = NO;
    nsWindow.backgroundColor = NSColor.clearColor;
    nsWindow.hasShadow = YES;   // Cocoa infers the shadow's shape from the opaque pixels left
                                // after masking, so this still reads as a soft rounded shadow.

    view.wantsLayer = YES;
    view.layer.cornerRadius = (CGFloat) cornerRadiusPx;
    view.layer.masksToBounds = YES;
}

void r3wrkBeginWindowDrag (void* componentPtr)
{
    auto* comp = static_cast<juce::Component*> (componentPtr);
    if (comp == nullptr)
        return;

    auto* peer = comp->getPeer();
    if (peer == nullptr)
        return;

    NSView* view = (NSView*) peer->getNativeHandle();
    NSWindow* nsWindow = view.window;
    NSEvent* ev = NSApp.currentEvent;
    if (nsWindow != nil && ev != nil)
        [nsWindow performWindowDragWithEvent: ev];   // AppKit takes over the move loop
}

// --- "Float on top", made sticky -------------------------------------------------------------
// Two problems, both handled here:
//  1. A plain NSFloatingWindowLevel window gets LEFT BEHIND when you switch Spaces and is
//     COVERED when another app goes full screen -- so the collectionBehavior below
//     (CanJoinAllSpaces + FullScreenAuxiliary) is what actually makes it follow you around.
//  2. macOS also silently drops the level, or sinks the window while the level stays numerically
//     correct, on ordinary operations (zoom, miniaturise+restore, full-screen toggles, a file
//     dialog opening, ...). r3wrkReassertFloatLevel() puts the level back AND re-lifts the
//     window with orderFrontRegardless (front of its level, no focus steal). It's called from
//     notification observers on the known triggers plus a 1 s keep-alive timer for the rest.
// Standalone has exactly one window for the life of the app, so a single slot is enough. This
// .mm is compiled without ARC, so the statics are unretained raw pointers -- fine, everything
// here outlives the app.
static NSWindow* g_floatWindow = nil;
static bool      g_floatObserversInstalled = false;
static NSTimer*  g_floatKeepAliveTimer = nil;

static const NSWindowCollectionBehavior kFloatCollectionBits =
    NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

static void r3wrkReassertFloatLevel()
{
    if (g_floatWindow == nil)
        return;
    if (g_floatWindow.level != NSFloatingWindowLevel)
        g_floatWindow.level = NSFloatingWindowLevel;
    if ((g_floatWindow.collectionBehavior & kFloatCollectionBits) != kFloatCollectionBits)
        g_floatWindow.collectionBehavior |= kFloatCollectionBits;
    [g_floatWindow orderFrontRegardless];   // re-lift without activating the app / stealing focus
}

void r3wrkTitleBarDoubleClick (void* componentPtr)
{
    auto* comp = static_cast<juce::Component*> (componentPtr);
    if (comp == nullptr)
        return;

    auto* peer = comp->getPeer();
    if (peer == nullptr)
        return;

    NSView* view = (NSView*) peer->getNativeHandle();
    NSWindow* nsWindow = view.window;
    if (nsWindow == nil)
        return;

    // Honour System Settings > Desktop & Dock > "Double-click a window's title bar to".
    // AppleActionOnDoubleClick lives in NSGlobalDomain: "Maximize" (zoom to fill the screen),
    // "Minimize", or "None"; absent means the default, Maximize.
    NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey: @"AppleActionOnDoubleClick"];

    if ([action isEqualToString: @"Minimize"])
        [nsWindow miniaturize: nil];
    else if (! [action isEqualToString: @"None"])
        [nsWindow zoom: nil];   // to the screen's visible frame; a second call restores

    r3wrkReassertFloatLevel();   // zoom resets the window level -- put it back if we're floating
}

void r3wrkSetWindowFloatOnTop (void* componentPtr, bool onTop)
{
    auto* comp = static_cast<juce::Component*> (componentPtr);
    if (comp == nullptr)
        return;

    auto* peer = comp->getPeer();
    if (peer == nullptr)
        return;

    NSView* view = (NSView*) peer->getNativeHandle();
    NSWindow* nsWindow = view.window;
    if (nsWindow == nil)
        return;

    if (onTop)
    {
        nsWindow.level = NSFloatingWindowLevel;
        nsWindow.collectionBehavior |= kFloatCollectionBits;   // follow across Spaces + over full-screen
        g_floatWindow = nsWindow;
        [nsWindow orderFrontRegardless];
    }
    else
    {
        nsWindow.level = NSNormalWindowLevel;
        nsWindow.collectionBehavior &= ~kFloatCollectionBits;
        g_floatWindow = nil;
    }

    if (onTop && ! g_floatObserversInstalled)
    {
        g_floatObserversInstalled = true;

        void (^reassert)(NSNotification*) = ^(NSNotification*) { r3wrkReassertFloatLevel(); };

        // Any of these can have just cleared the level; re-assert once each settles. Scoped to
        // no particular object (there's only our window + transient panels), and a no-op while
        // g_floatWindow is nil, so leaving them registered when Float is off costs nothing.
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        for (NSNotificationName n in @[ NSWindowDidBecomeKeyNotification,
                                       NSWindowDidResignKeyNotification,
                                       NSWindowDidDeminiaturizeNotification,
                                       NSWindowDidResizeNotification,
                                       NSWindowDidMoveNotification,
                                       NSWindowDidEndLiveResizeNotification,
                                       NSWindowDidEnterFullScreenNotification,
                                       NSWindowDidExitFullScreenNotification,
                                       NSWindowDidChangeScreenNotification,
                                       NSApplicationDidBecomeActiveNotification,
                                       NSApplicationDidResignActiveNotification,
                                       NSApplicationDidUnhideNotification ])
            [nc addObserverForName: n object: nil
                            queue: [NSOperationQueue mainQueue] usingBlock: reassert];

        // Spaces / Mission Control changes come through NSWorkspace's own centre, not the
        // default one -- moving the window between Spaces is a known level-reset trigger.
        [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserverForName: NSWorkspaceActiveSpaceDidChangeNotification object: nil
                        queue: [NSOperationQueue mainQueue] usingBlock: reassert];

        // Catch-all: a 1 s keep-alive for triggers not covered above. Added to the common run
        // loop modes so it keeps firing during menu tracking / live resize / a modal file
        // panel, which is exactly when the level tends to get reset. r3wrkReassertFloatLevel()
        // no-ops unless the level actually drifted, so this is cheap and glitch-free.
        g_floatKeepAliveTimer = [NSTimer timerWithTimeInterval: 1.0
                                                       repeats: YES
                                                         block: ^(NSTimer*) { r3wrkReassertFloatLevel(); }];
        [[NSRunLoop mainRunLoop] addTimer: g_floatKeepAliveTimer forMode: NSRunLoopCommonModes];
    }
}
#endif
