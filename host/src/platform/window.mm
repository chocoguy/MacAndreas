#include "window.hpp"

#include "../input/input_state.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstdio>
#include <map>

namespace {

constexpr uint32_t kWmActivate = 0x0006;
constexpr uint32_t kWmSetFocus = 0x0007;
constexpr uint32_t kWmKillFocus = 0x0008;
constexpr uint32_t kWmClose = 0x0010;
constexpr uint32_t kWmActivateApp = 0x001c;

ma::host::platform::PostFn g_post;
ma::host::platform::KeyFn g_key;

constexpr NSEventModifierFlags kDeviceLCtrl = 0x0001;
constexpr NSEventModifierFlags kDeviceLShift = 0x0002;
constexpr NSEventModifierFlags kDeviceRShift = 0x0004;
constexpr NSEventModifierFlags kDeviceLAlt = 0x0020;
constexpr NSEventModifierFlags kDeviceRAlt = 0x0040;
constexpr NSEventModifierFlags kDeviceRCtrl = 0x2000;
constexpr uint16_t kMacCapsLock = 0x39;

NSEventModifierFlags device_mask(uint16_t mac) {
    switch (mac) {
    case 0x3b: return kDeviceLCtrl;
    case 0x38: return kDeviceLShift;
    case 0x3c: return kDeviceRShift;
    case 0x3a: return kDeviceLAlt;
    case 0x3d: return kDeviceRAlt;
    case 0x3e: return kDeviceRCtrl;
    default: return 0;
    }
}

void key(uint32_t hwnd, uint16_t mac, bool down, bool repeat) {
    const ma::host::input::Key* k = ma::host::input::key_for_mac(mac);
    if (k && g_key) {
        g_key(hwnd, *k, down, repeat);
    }
}

void post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    if (g_post) {
        g_post(hwnd, msg, wparam, lparam);
    }
}

}

@interface MAView : NSView
@property(nonatomic) uint32_t hwnd;
@end

@implementation MAView
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    }
    return self;
}
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    return layer;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)updateDrawableSize {
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    CGFloat scale = self.window ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    layer.contentsScale = scale;
    NSSize size = self.bounds.size;
    layer.drawableSize = CGSizeMake(std::max(1.0, std::round(size.width * scale)), std::max(1.0, std::round(size.height * scale)));
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}
- (void)mouseMoved:(NSEvent*)e {
    ma::host::input::add_mouse_motion(static_cast<int32_t>(e.deltaX), static_cast<int32_t>(e.deltaY));
}
- (void)mouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
- (void)rightMouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
- (void)otherMouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
- (void)mouseDown:(NSEvent*)e {
    ma::host::input::set_mouse_button(0, true);
}
- (void)mouseUp:(NSEvent*)e {
    ma::host::input::set_mouse_button(0, false);
}
- (void)rightMouseDown:(NSEvent*)e {
    ma::host::input::set_mouse_button(1, true);
}
- (void)rightMouseUp:(NSEvent*)e {
    ma::host::input::set_mouse_button(1, false);
}
- (void)otherMouseDown:(NSEvent*)e {
    ma::host::input::set_mouse_button(static_cast<unsigned>(e.buttonNumber), true);
}
- (void)otherMouseUp:(NSEvent*)e {
    ma::host::input::set_mouse_button(static_cast<unsigned>(e.buttonNumber), false);
}
- (void)scrollWheel:(NSEvent*)e {
    ma::host::input::add_mouse_wheel(static_cast<int32_t>(e.scrollingDeltaY * 40));
}
- (void)keyDown:(NSEvent*)e {
    if (e.modifierFlags & NSEventModifierFlagCommand) {
        return;
    }
    key(self.hwnd, e.keyCode, true, e.ARepeat);
}
- (void)keyUp:(NSEvent*)e {
    key(self.hwnd, e.keyCode, false, false);
}
- (void)flagsChanged:(NSEvent*)e {
    if (e.keyCode == kMacCapsLock) {
        key(self.hwnd, e.keyCode, true, false);
        key(self.hwnd, e.keyCode, false, false);
        return;
    }
    NSEventModifierFlags mask = device_mask(e.keyCode);
    if (mask != 0) {
        key(self.hwnd, e.keyCode, (e.modifierFlags & mask) != 0, false);
    }
}
@end

@interface MAWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) uint32_t hwnd;
@end

@implementation MAWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    post(self.hwnd, kWmClose, 0, 0);
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification*)n {
    post(self.hwnd, kWmActivateApp, 1, 0);
    post(self.hwnd, kWmActivate, 1, 0);
    post(self.hwnd, kWmSetFocus, 0, 0);
}
- (void)windowDidResignKey:(NSNotification*)n {
    ma::host::input::reset();
    post(self.hwnd, kWmKillFocus, 0, 0);
    post(self.hwnd, kWmActivate, 0, 0);
    post(self.hwnd, kWmActivateApp, 0, 0);
}
@end

namespace ma::host::platform {

namespace {

struct Entry {
    NSWindow* window = nil;
    MAView* view = nil;
    MAWindowDelegate* delegate = nil;
};

std::map<uint32_t, Entry>& windows() {
    static std::map<uint32_t, Entry> w;
    return w;
}

void ensure_app() {
    static bool ready = false;
    if (ready) {
        return;
    }
    ready = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:@"Quit MacAndreas" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
}

NSSize points_for_pixels(uint32_t width, uint32_t height) {
    CGFloat scale = NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor : 1.0;
    NSSize size = NSMakeSize(width / scale, height / scale);
    if (NSScreen.mainScreen) {
        NSRect visible = NSScreen.mainScreen.visibleFrame;
        CGFloat fit = std::min({1.0, visible.size.width / size.width, (visible.size.height - 32) / size.height});
        size.width *= fit;
        size.height *= fit;
    }
    return size;
}

}

void set_post_callback(PostFn fn) { g_post = std::move(fn); }

void set_key_callback(KeyFn fn) { g_key = std::move(fn); }

bool has_window_server() {
    static const bool ok = [] {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session) {
            return false;
        }
        CFRelease(session);
        return true;
    }();
    return ok;
}

void create_window(uint32_t hwnd, const std::string& title, uint32_t width, uint32_t height) {
    @autoreleasepool {
        ensure_app();
        NSSize size = points_for_pixels(width, height);
        NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                            NSWindowStyleMaskMiniaturizable |
                                                            NSWindowStyleMaskResizable
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w.releasedWhenClosed = NO;
        w.title = [NSString stringWithUTF8String:title.c_str()];
        w.acceptsMouseMovedEvents = YES;
        w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        MAView* view = [[MAView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
        view.hwnd = hwnd;
        w.contentView = view;
        [view updateDrawableSize];
        MAWindowDelegate* delegate = [[MAWindowDelegate alloc] init];
        delegate.hwnd = hwnd;
        w.delegate = delegate;
        [w center];
        windows()[hwnd] = Entry{w, view, delegate};
    }
}

void destroy_window(uint32_t hwnd) {
    auto it = windows().find(hwnd);
    if (it == windows().end()) {
        return;
    }
    it->second.window.delegate = nil;
    [it->second.window close];
    windows().erase(it);
}

void show_window(uint32_t hwnd, bool show) {
    auto it = windows().find(hwnd);
    if (it == windows().end()) {
        return;
    }
    if (show) {
        [it->second.window makeKeyAndOrderFront:nil];
        [NSApp activate];
    } else {
        [it->second.window orderOut:nil];
    }
}

void set_title(uint32_t hwnd, const std::string& title) {
    auto it = windows().find(hwnd);
    if (it != windows().end()) {
        it->second.window.title = [NSString stringWithUTF8String:title.c_str()];
    }
}

void set_client_pixels(uint32_t hwnd, uint32_t width, uint32_t height) {
    auto it = windows().find(hwnd);
    if (it == windows().end() || width == 0 || height == 0) {
        return;
    }
    NSSize size = points_for_pixels(width, height);
    [it->second.window setContentSize:size];
    [it->second.window center];
    [it->second.view updateDrawableSize];
}

void* metal_layer(uint32_t hwnd) {
    auto it = windows().find(hwnd);
    if (it == windows().end()) {
        return nullptr;
    }
    return (__bridge void*)it->second.view.layer;
}

void pump_events() {
    if (!NSApp) {
        return;
    }
    @autoreleasepool {
        while (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:nil
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES]) {
            [NSApp sendEvent:e];
        }
    }
}

}
