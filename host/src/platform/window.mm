#include "window.hpp"

#include "../input/input_state.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstdio>
#include <map>

namespace {

constexpr uint32_t kWmClose = 0x0010;

ma::host::platform::PostFn g_post;
ma::host::platform::InputCallbacks g_input;
bool g_cursor_hidden = false;

NSCursor* blank_cursor() {
    static NSCursor* cursor = [] {
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        return [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
    }();
    return cursor;
}

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
    if (k && g_input.key) {
        g_input.key(hwnd, *k, down, repeat);
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
@property(nonatomic) uint32_t clientWidth;
@property(nonatomic) uint32_t clientHeight;
@property(nonatomic) double wheelRemainder;
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
- (NSPoint)clientPoint:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSSize size = self.bounds.size;
    if (size.width <= 0 || size.height <= 0) {
        return NSZeroPoint;
    }
    return NSMakePoint(std::floor(p.x / size.width * self.clientWidth),
                       std::floor((size.height - p.y) / size.height * self.clientHeight));
}
- (void)resetCursorRects {
    if (g_cursor_hidden) {
        [self addCursorRect:self.bounds cursor:blank_cursor()];
    }
}
- (void)mouseMoved:(NSEvent*)e {
    ma::host::input::add_mouse_motion(static_cast<int32_t>(e.deltaX), static_cast<int32_t>(e.deltaY));
    NSPoint p = [self clientPoint:e];
    bool inside = p.x >= 0 && p.y >= 0 && p.x < self.clientWidth && p.y < self.clientHeight;
    bool held = NSEvent.pressedMouseButtons != 0;
    if (g_input.move && (inside || held)) {
        g_input.move(self.hwnd, static_cast<int32_t>(p.x), static_cast<int32_t>(p.y));
    }
}
- (void)button:(unsigned)button down:(bool)down event:(NSEvent*)e {
    if (g_input.button) {
        [self mouseMoved:e];
        g_input.button(self.hwnd, button, down);
    } else {
        ma::host::input::set_mouse_button(button, down);
    }
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
    [self button:0 down:true event:e];
}
- (void)mouseUp:(NSEvent*)e {
    [self button:0 down:false event:e];
}
- (void)rightMouseDown:(NSEvent*)e {
    [self button:1 down:true event:e];
}
- (void)rightMouseUp:(NSEvent*)e {
    [self button:1 down:false event:e];
}
- (void)otherMouseDown:(NSEvent*)e {
    [self button:static_cast<unsigned>(e.buttonNumber) down:true event:e];
}
- (void)otherMouseUp:(NSEvent*)e {
    [self button:static_cast<unsigned>(e.buttonNumber) down:false event:e];
}
- (void)scrollWheel:(NSEvent*)e {
    double notches = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
    double total = notches * 120.0 + self.wheelRemainder;
    int32_t delta = static_cast<int32_t>(std::trunc(total));
    self.wheelRemainder = total - delta;
    if (delta == 0) {
        return;
    }
    if (g_input.wheel) {
        g_input.wheel(self.hwnd, delta);
    } else {
        ma::host::input::add_mouse_wheel(delta);
    }
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
    if (g_input.focus) {
        g_input.focus(self.hwnd, true);
    }
}
- (void)windowDidResignKey:(NSNotification*)n {
    if (g_input.focus) {
        g_input.focus(self.hwnd, false);
    }
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

void set_input_callbacks(InputCallbacks callbacks) { g_input = std::move(callbacks); }

void set_cursor_hidden(bool hidden) {
    if (g_cursor_hidden == hidden) {
        return;
    }
    g_cursor_hidden = hidden;
    for (auto& [hwnd, entry] : windows()) {
        [entry.window invalidateCursorRectsForView:entry.view];
    }
}

void warp_cursor(uint32_t hwnd, int32_t x, int32_t y) {
    auto it = windows().find(hwnd);
    if (it == windows().end() || !NSApp.active || !it->second.window.keyWindow) {
        return;
    }
    MAView* view = it->second.view;
    NSWindow* window = it->second.window;
    NSPoint inside = [view convertPoint:window.mouseLocationOutsideOfEventStream fromView:nil];
    if (!NSPointInRect(inside, view.bounds) || view.clientWidth == 0 || view.clientHeight == 0) {
        return;
    }
    NSSize size = view.bounds.size;
    NSPoint local = NSMakePoint((x + 0.5) * size.width / view.clientWidth,
                                size.height - (y + 0.5) * size.height / view.clientHeight);
    NSPoint screen = [window convertPointToScreen:[view convertPoint:local toView:nil]];
    CGFloat top = NSScreen.screens.firstObject.frame.size.height;
    CGWarpMouseCursorPosition(CGPointMake(screen.x, top - screen.y));
    CGAssociateMouseAndMouseCursorPosition(true);
}

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
        view.clientWidth = width;
        view.clientHeight = height;
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
    it->second.view.clientWidth = width;
    it->second.view.clientHeight = height;
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
