// SPDX-License-Identifier: GPL-3.0-or-later
#include "MacApp.h"

#import <AppKit/AppKit.h>

#include <QGuiApplication>
#include <QWindow>

void macActivateApp()
{
    if (@available(macOS 14.0, *)) {
        // Cooperative activation: macOS may decline it when the user is busy in another application.
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

void macSetDockBadge(const QString &label)
{
    // NSApp is nil when Qt runs without the cocoa platform (offscreen): messages to nil do nothing.
    NSString *text = label.isEmpty() ? nil : label.toNSString();
    NSApp.dockTile.badgeLabel = text;
}

void macSetDockIconHidden(bool hidden)
{
    const NSApplicationActivationPolicy policy = hidden ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular;
    if (NSApp && NSApp.activationPolicy != policy) {
        [NSApp setActivationPolicy:policy];
    }
}

QuitPolicy::Origin macCurrentQuitOrigin()
{
    NSAppleEventDescriptor *event = [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
    if (!event || event.eventClass != kCoreEventClass || event.eventID != kAEQuitApplication) {
        return QuitPolicy::Origin::Application;
    }
    // The login window gives the reason as an attribute; look at the parameters as well.
    NSAppleEventDescriptor *reason = [event attributeDescriptorForKeyword:kAEQuitReason];
    if (!reason) {
        reason = [event paramDescriptorForKeyword:kAEQuitReason];
    }
    return reason && reason.typeCodeValue != 0 ? QuitPolicy::Origin::SessionEnding : QuitPolicy::Origin::External;
}

void macSetUpTitleBar(QWindow *window)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("cocoa")) {
        return;
    }
    // With the cocoa platform the native handle is the window's NSView.
    auto *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    NSWindow *nsWindow = view.window;
    nsWindow.titleVisibility = NSWindowTitleHidden;
    // Without a toolbar the lights sit near the top of a strip lower than Sugo's title bar; an
    // empty toolbar makes the strip that of a compact toolbar, with the lights in its middle.
    // The toolbar has no items and draws nothing (the title bar is transparent): clicks and
    // drags on it still reach the content, i.e. the title bar.
    if (!nsWindow.toolbar) {
        NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"stream.sugo.app.titlebar"];
        toolbar.allowsUserCustomization = NO;
        nsWindow.toolbar = toolbar;
        // In full screen the system would keep the toolbar shown, in a window of its own over the
        // top of the content, where it would take the clicks meant for the title bar's tabs; the
        // traffic lights are hidden there anyway. Hidden once the window is full screen, back once
        // it has left it: only a change that succeeded sends these (a failed one would leave a
        // "will" without its "did"). (The window lives as long as the application: the
        // observers are never removed.)
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        __weak NSToolbar *weakToolbar = toolbar;
        [center addObserverForName:NSWindowDidEnterFullScreenNotification object:nsWindow queue:nil
                        usingBlock:^(NSNotification *) { weakToolbar.visible = NO; }];
        [center addObserverForName:NSWindowDidExitFullScreenNotification object:nsWindow queue:nil
                        usingBlock:^(NSNotification *) { weakToolbar.visible = YES; }];
    }
    nsWindow.toolbarStyle = NSWindowToolbarStyleUnifiedCompact;
    nsWindow.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
}

void macStopTrayActivationReports()
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) {
        return;
    }
    // Qt's status item delegate is the target of the icon's button and the observer of the
    // menu's NSMenuDidBeginTrackingNotification; without that observer the menu still opens (it
    // belongs to the status item), only the report of the click goes.
    const SEL clicked = NSSelectorFromString(@"statusItemClicked");
    for (NSWindow *window in NSApp.windows) {
        if (!window.contentView) {
            continue;
        }
        NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObject:window.contentView];
        while (views.count) {
            NSView *view = views.lastObject;
            [views removeLastObject];
            if ([view isKindOfClass:[NSStatusBarButton class]]) {
                NSStatusBarButton *button = (NSStatusBarButton *)view;
                if (button.action == clicked && button.target) {
                    [NSNotificationCenter.defaultCenter removeObserver:button.target
                                                                  name:NSMenuDidBeginTrackingNotification
                                                                object:nil];
                }
            }
            [views addObjectsFromArray:view.subviews];
        }
    }
}
