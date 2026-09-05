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
#endif
