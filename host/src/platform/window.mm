#include "window.hpp"
#include "viewport.hpp"

#include "../audio/output.hpp"
#include "../input/input_state.hpp"
#include "../perf.hpp"

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace {

constexpr uint32_t kWmClose = 0x0010;

ma::host::platform::PostFn g_post;
ma::host::platform::InputCallbacks g_input;
bool g_cursor_hidden = false;
bool g_user_input = true;

constexpr double kRawMotionHold = 0.25;
std::atomic<bool> g_raw_motion_allowed{false};
std::atomic<double> g_last_raw_motion{-1.0e9};

bool raw_motion_recent() { return CACurrentMediaTime() - g_last_raw_motion.load() < kRawMotionHold; }

void watch_mouse(GCMouse* mouse) {
    static dispatch_queue_t queue = dispatch_queue_create("MacAndreas.mouse", DISPATCH_QUEUE_SERIAL);
    mouse.handlerQueue = queue;
    mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput*, float dx, float dy) {
      if (!g_raw_motion_allowed.load()) {
          return;
      }
      g_last_raw_motion.store(CACurrentMediaTime());
      ma::host::input::add_mouse_motion(dx, -dy);
    };
    NSString* name = mouse.vendorName ?: @"mouse";
    std::fprintf(stderr, "[INPUT] raw mouse connected: %s\n", name.UTF8String);
}

void start_raw_mouse() {
    [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification* n) {
                                                  watch_mouse(n.object);
                                                }];
    for (GCMouse* mouse in GCMouse.mice) {
        watch_mouse(mouse);
    }
}

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
    if (!g_user_input) {
        return;
    }
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
- (ma::host::platform::ContentRect)contentRect;
@end

@interface MAWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) uint32_t hwnd;
@end

@interface MAWindow : NSWindow
@end

@interface MAAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) bool quitRequested;
@end

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

struct Capture {
    bool active = false;
    bool suppressed = false;
    uint32_t hwnd = 0;
    double x = 0;
    double y = 0;
};

Capture g_capture;

void warp_to(const Entry& e, double x, double y) {
    MAView* view = e.view;
    auto p = ma::host::platform::client_to_view([view contentRect], x + 0.5, y + 0.5, view.clientWidth,
                                                view.clientHeight);
    NSPoint screen = [e.window convertPointToScreen:[view convertPoint:NSMakePoint(p.x, p.y) toView:nil]];
    CGFloat top = NSScreen.screens.firstObject.frame.size.height;
    CGWarpMouseCursorPosition(CGPointMake(screen.x, top - screen.y));
}

bool capture_wanted(const Entry& e) {
    return g_user_input && ma::host::settings().mouse_capture && g_cursor_hidden && !g_capture.suppressed &&
           NSApp.active && e.window.keyWindow && e.view.clientWidth != 0 && e.view.clientHeight != 0;
}

void release_capture(bool warp) {
    if (!g_capture.active) {
        return;
    }
    g_capture.active = false;
    auto it = windows().find(g_capture.hwnd);
    if (warp && it != windows().end()) {
        warp_to(it->second, g_capture.x, g_capture.y);
    }
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    if (it != windows().end()) {
        [it->second.window invalidateCursorRectsForView:it->second.view];
    }
    std::fprintf(stderr, "[INPUT] mouse released\n");
}

void engage_capture(uint32_t hwnd, const Entry& e) {
    MAView* view = e.view;
    uint32_t cw = view.clientWidth;
    uint32_t ch = view.clientHeight;
    NSPoint p = [view convertPoint:e.window.mouseLocationOutsideOfEventStream fromView:nil];
    auto c = ma::host::platform::view_to_client([view contentRect], p.x, p.y, cw, ch);
    if (c.x < 0 || c.y < 0 || c.x >= cw || c.y >= ch) {
        c = {cw / 2.0, ch / 2.0};
    }
    g_capture.active = true;
    g_capture.hwnd = hwnd;
    g_capture.x = c.x;
    g_capture.y = c.y;
    warp_to(e, c.x, c.y);
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    [e.window invalidateCursorRectsForView:view];
    std::fprintf(stderr, "[INPUT] mouse captured; press Command to release it\n");
    if (g_input.move) {
        g_input.move(hwnd, static_cast<int32_t>(c.x), static_cast<int32_t>(c.y));
    }
}

void update_capture() {
    if (g_capture.active) {
        auto it = windows().find(g_capture.hwnd);
        if (it == windows().end()) {
            g_capture.active = false;
            CGAssociateMouseAndMouseCursorPosition(true);
            [NSCursor unhide];
            return;
        }
        if (!capture_wanted(it->second)) {
            release_capture(NSApp.active && it->second.window.keyWindow);
        }
        return;
    }
    if (g_capture.suppressed || !g_user_input || !g_cursor_hidden || !ma::host::settings().mouse_capture) {
        return;
    }
    for (auto& [hwnd, e] : windows()) {
        if (capture_wanted(e)) {
            engage_capture(hwnd, e);
            return;
        }
    }
}

}

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
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
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
- (ma::host::platform::ContentRect)contentRect {
    NSSize size = self.bounds.size;
    return ma::host::platform::fit_content(size.width, size.height, self.clientWidth, self.clientHeight);
}
- (NSPoint)clientPoint:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    auto c = ma::host::platform::view_to_client([self contentRect], p.x, p.y, self.clientWidth, self.clientHeight);
    return NSMakePoint(std::floor(c.x), std::floor(c.y));
}
- (void)resetCursorRects {
    if (g_cursor_hidden && !ma::host::settings().mouse_capture) {
        [self addCursorRect:self.bounds cursor:blank_cursor()];
    }
}
- (void)mouseMoved:(NSEvent*)e {
    if (!g_user_input) {
        return;
    }
    if (!raw_motion_recent()) {
        CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
        ma::host::input::add_mouse_motion(e.deltaX * scale, e.deltaY * scale);
    }
    if (g_capture.active) {
        if (g_capture.hwnd != self.hwnd) {
            return;
        }
        auto r = [self contentRect];
        if (r.width > 0 && r.height > 0) {
            auto c = ma::host::platform::clamp_client({g_capture.x + e.deltaX * self.clientWidth / r.width,
                                                       g_capture.y + e.deltaY * self.clientHeight / r.height},
                                                      self.clientWidth, self.clientHeight);
            g_capture.x = c.x;
            g_capture.y = c.y;
        }
        if (g_input.move) {
            g_input.move(self.hwnd, static_cast<int32_t>(g_capture.x), static_cast<int32_t>(g_capture.y));
        }
        return;
    }
    NSPoint p = [self clientPoint:e];
    bool inside = p.x >= 0 && p.y >= 0 && p.x < self.clientWidth && p.y < self.clientHeight;
    bool held = NSEvent.pressedMouseButtons != 0;
    if (g_input.move && (inside || held)) {
        g_input.move(self.hwnd, static_cast<int32_t>(p.x), static_cast<int32_t>(p.y));
    }
}
- (void)button:(unsigned)button down:(bool)down event:(NSEvent*)e {
    if (!g_user_input) {
        return;
    }
    if (down && !g_capture.active && g_capture.suppressed) {
        g_capture.suppressed = false;
        update_capture();
    }
    if (g_input.button) {
        if (!g_capture.active) {
            [self mouseMoved:e];
        }
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
    if (!g_user_input) {
        return;
    }
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
    if ((e.modifierFlags & NSEventModifierFlagCommand) && g_capture.active) {
        g_capture.suppressed = true;
        release_capture(true);
    }
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

@implementation MAAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (!self.quitRequested && !windows().empty()) {
        self.quitRequested = true;
        std::fprintf(stderr, "[WINDOW] quit requested; closing the game window\n");
        post(windows().begin()->first, kWmClose, 0, 0);
        return NSTerminateCancel;
    }
    std::fprintf(stderr, "[WINDOW] quitting immediately\n");
    ma::host::audio::finish_output();
    ma::host::perf::finish();
    std::fflush(stderr);
    std::_Exit(0);
}
@end

@implementation MAWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

@implementation MAWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    post(self.hwnd, kWmClose, 0, 0);
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification*)n {
    g_capture.suppressed = false;
    if (g_input.focus) {
        g_input.focus(self.hwnd, true);
    }
}
- (void)windowDidResignKey:(NSNotification*)n {
    update_capture();
    if (g_input.focus) {
        g_input.focus(self.hwnd, false);
    }
}
- (void)windowDidEnterFullScreen:(NSNotification*)n {
    auto it = windows().find(self.hwnd);
    if (it != windows().end()) {
        CGSize drawable = ((CAMetalLayer*)it->second.view.layer).drawableSize;
        std::fprintf(stderr, "[WINDOW] entered full screen, drawable %.0fx%.0f\n", drawable.width, drawable.height);
    }
}
- (void)windowDidExitFullScreen:(NSNotification*)n {
    std::fprintf(stderr, "[WINDOW] left full screen\n");
}
- (void)windowDidChangeScreen:(NSNotification*)n {
    auto it = windows().find(self.hwnd);
    if (it != windows().end()) {
        [it->second.view updateDrawableSize];
    }
}
@end

namespace ma::host::platform {

namespace {

void ensure_app() {
    static bool ready = false;
    if (ready) {
        return;
    }
    ready = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    static MAAppDelegate* app_delegate = [[MAAppDelegate alloc] init];
    NSApp.delegate = app_delegate;
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    if (settings().window_mode != WindowMode::borderless) {
        NSMenuItem* full = [app_menu addItemWithTitle:@"Toggle Full Screen"
                                               action:@selector(toggleFullScreen:)
                                        keyEquivalent:@"f"];
        full.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
        [app_menu addItem:[NSMenuItem separatorItem]];
    }
    [app_menu addItemWithTitle:@"Quit MacAndreas" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
    start_raw_mouse();
}

NSScreen* target_screen() {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (settings().display < screens.count) {
        return screens[settings().display];
    }
    return screens.firstObject;
}

NSRect safe_frame(NSScreen* screen) {
    NSRect frame = screen.frame;
    NSEdgeInsets inset = screen.safeAreaInsets;
    frame.size.height -= inset.top + inset.bottom;
    frame.size.width -= inset.left + inset.right;
    frame.origin.x += inset.left;
    frame.origin.y += inset.bottom;
    return frame;
}

NSSize points_for_pixels(NSScreen* screen, uint32_t width, uint32_t height) {
    CGFloat scale = screen ? screen.backingScaleFactor : 1.0;
    NSSize size = NSMakeSize(width / scale, height / scale);
    if (screen) {
        NSRect visible = screen.visibleFrame;
        CGFloat fit = std::min({1.0, visible.size.width / size.width, (visible.size.height - 32) / size.height});
        size.width *= fit;
        size.height *= fit;
    }
    return size;
}

void center_on(NSWindow* w, NSScreen* screen) {
    if (!screen) {
        [w center];
        return;
    }
    NSRect visible = screen.visibleFrame;
    NSRect frame = w.frame;
    [w setFrameOrigin:NSMakePoint(std::round(visible.origin.x + (visible.size.width - frame.size.width) / 2),
                                  std::round(visible.origin.y + (visible.size.height - frame.size.height) / 2))];
}

bool resizes_with_game(NSWindow* w) {
    return settings().window_mode != WindowMode::borderless && !(w.styleMask & NSWindowStyleMaskFullScreen);
}

}

std::vector<DisplayInfo> displays() {
    std::vector<DisplayInfo> out;
    @autoreleasepool {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        for (NSUInteger i = 0; i < screens.count; ++i) {
            NSScreen* s = screens[i];
            NSRect frame = safe_frame(s);
            double scale = s.backingScaleFactor;
            out.push_back({s.localizedName ? s.localizedName.UTF8String : "Display",
                           static_cast<uint32_t>(std::lround(frame.size.width * scale)),
                           static_cast<uint32_t>(std::lround(frame.size.height * scale)), scale, i == 0});
        }
    }
    return out;
}

DisplayMode display_pixels(uint32_t index) {
    auto list = displays();
    if (list.empty()) {
        return {};
    }
    const DisplayInfo& d = index < list.size() ? list[index] : list.front();
    return {d.width, d.height};
}

void set_post_callback(PostFn fn) { g_post = std::move(fn); }

void set_input_callbacks(InputCallbacks callbacks) { g_input = std::move(callbacks); }

void set_user_input(bool enabled) { g_user_input = enabled; }

bool mouse_captured() { return g_capture.active; }

void set_cursor_hidden(bool hidden) {
    if (g_cursor_hidden == hidden) {
        return;
    }
    g_cursor_hidden = hidden;
    for (auto& [hwnd, entry] : windows()) {
        [entry.window invalidateCursorRectsForView:entry.view];
    }
    if (NSApp) {
        update_capture();
    }
}

void warp_cursor(uint32_t hwnd, int32_t x, int32_t y) {
    auto it = windows().find(hwnd);
    if (it == windows().end()) {
        return;
    }
    MAView* view = it->second.view;
    if (g_capture.active) {
        if (g_capture.hwnd == hwnd) {
            auto c = clamp_client({static_cast<double>(x), static_cast<double>(y)}, view.clientWidth, view.clientHeight);
            g_capture.x = c.x;
            g_capture.y = c.y;
        }
        return;
    }
    NSWindow* window = it->second.window;
    if (!g_user_input || !NSApp.active || !window.keyWindow) {
        return;
    }
    NSPoint inside = [view convertPoint:window.mouseLocationOutsideOfEventStream fromView:nil];
    if (!NSPointInRect(inside, view.bounds) || view.clientWidth == 0 || view.clientHeight == 0) {
        return;
    }
    warp_to(it->second, x, y);
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
        NSScreen* screen = target_screen();
        bool borderless = settings().window_mode == WindowMode::borderless && screen;
        NSWindow* w = nil;
        MAView* view = nil;
        if (borderless) {
            NSRect frame = screen.frame;
            w = [[MAWindow alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskBorderless
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
            [w setFrame:frame display:NO];
            NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
            root.wantsLayer = YES;
            root.layer.backgroundColor = NSColor.blackColor.CGColor;
            NSRect safe = safe_frame(screen);
            view = [[MAView alloc] initWithFrame:NSMakeRect(safe.origin.x - frame.origin.x, safe.origin.y - frame.origin.y,
                                                            safe.size.width, safe.size.height)];
            [root addSubview:view];
            w.contentView = root;
            w.backgroundColor = NSColor.blackColor;
            w.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone | NSWindowCollectionBehaviorManaged;
            NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
        } else {
            NSSize size = points_for_pixels(screen, width, height);
            w = [[MAWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO
                                               screen:screen];
            view = [[MAView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
            w.contentView = view;
            w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
            w.backgroundColor = NSColor.blackColor;
            center_on(w, screen);
        }
        w.releasedWhenClosed = NO;
        w.title = [NSString stringWithUTF8String:title.c_str()];
        w.acceptsMouseMovedEvents = YES;
        view.hwnd = hwnd;
        view.clientWidth = width;
        view.clientHeight = height;
        [view updateDrawableSize];
        MAWindowDelegate* delegate = [[MAWindowDelegate alloc] init];
        delegate.hwnd = hwnd;
        w.delegate = delegate;
        windows()[hwnd] = Entry{w, view, delegate};
        CGSize drawable = ((CAMetalLayer*)view.layer).drawableSize;
        std::fprintf(stderr, "[WINDOW] %s on %s: game %ux%u, view %.0fx%.0f points, drawable %.0fx%.0f\n",
                     std::string(window_mode_name(settings().window_mode)).c_str(),
                     screen && screen.localizedName ? screen.localizedName.UTF8String : "no display", width, height,
                     view.bounds.size.width, view.bounds.size.height, drawable.width, drawable.height);
    }
}

void destroy_window(uint32_t hwnd) {
    auto it = windows().find(hwnd);
    if (it == windows().end()) {
        return;
    }
    if (g_capture.active && g_capture.hwnd == hwnd) {
        release_capture(false);
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
    NSWindow* w = it->second.window;
    if (show) {
        bool first = !w.visible;
        [w makeKeyAndOrderFront:nil];
        [w makeFirstResponder:it->second.view];
        [NSApp activate];
        if (first && settings().window_mode == WindowMode::fullscreen && !(w.styleMask & NSWindowStyleMaskFullScreen)) {
            [w toggleFullScreen:nil];
        }
    } else {
        [w orderOut:nil];
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
    MAView* view = it->second.view;
    NSWindow* w = it->second.window;
    bool changed = view.clientWidth != width || view.clientHeight != height;
    view.clientWidth = width;
    view.clientHeight = height;
    if (g_capture.active && g_capture.hwnd == hwnd) {
        auto c = clamp_client({g_capture.x, g_capture.y}, width, height);
        g_capture.x = c.x;
        g_capture.y = c.y;
    }
    if (resizes_with_game(w) && changed) {
        NSScreen* screen = w.screen ? w.screen : target_screen();
        [w setContentSize:points_for_pixels(screen, width, height)];
        center_on(w, screen);
    }
    [view updateDrawableSize];
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
        update_capture();
        bool focused = false;
        for (auto& [hwnd, entry] : windows()) {
            focused = focused || entry.window.keyWindow;
        }
        g_raw_motion_allowed.store(g_user_input && NSApp.active && focused);
    }
}

}
