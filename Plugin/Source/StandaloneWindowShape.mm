// Cocoa first, JUCE second -- Carbon's Components.h (pulled in transitively by Cocoa.h) declares
// a plain global `Component` typedef that collides with juce::Component once JUCE's own headers
// bring that name into scope, the same ordering JUCE's own native .mm files always use.
#import <Cocoa/Cocoa.h>
#include "StandaloneWindowShape.h"

#if JUCE_MAC

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
}
#endif
