#include "gamepad.hpp"

#include "input_state.hpp"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <cstdio>

namespace ma::host::input {

namespace {

dispatch_queue_t g_queue;
GCController* g_active;

bool down(GCControllerButtonInput* b) { return b != nil && b.pressed; }

void snapshot(GCExtendedGamepad* gp) {
    PadState s;
    s.connected = true;
    s.lx = gp.leftThumbstick.xAxis.value;
    s.ly = gp.leftThumbstick.yAxis.value;
    s.rx = gp.rightThumbstick.xAxis.value;
    s.ry = gp.rightThumbstick.yAxis.value;
    s.set(PadButton::south, down(gp.buttonA));
    s.set(PadButton::east, down(gp.buttonB));
    s.set(PadButton::west, down(gp.buttonX));
    s.set(PadButton::north, down(gp.buttonY));
    s.set(PadButton::lb, down(gp.leftShoulder));
    s.set(PadButton::rb, down(gp.rightShoulder));
    s.set(PadButton::lt, down(gp.leftTrigger));
    s.set(PadButton::rt, down(gp.rightTrigger));
    s.set(PadButton::back, down(gp.buttonOptions));
    s.set(PadButton::start, down(gp.buttonMenu));
    s.set(PadButton::ls, down(gp.leftThumbstickButton));
    s.set(PadButton::rs, down(gp.rightThumbstickButton));
    s.set(PadButton::up, down(gp.dpad.up));
    s.set(PadButton::down, down(gp.dpad.down));
    s.set(PadButton::left, down(gp.dpad.left));
    s.set(PadButton::right, down(gp.dpad.right));
    set_device_pad(s);
}

const char* name_of(GCController* c) {
    NSString* name = c.vendorName ?: c.productCategory ?: @"controller";
    return name.UTF8String;
}

bool attach(GCController* c) {
    GCExtendedGamepad* gp = c.extendedGamepad;
    if (gp == nil) {
        return false;
    }
    g_active = c;
    c.handlerQueue = g_queue;
    gp.valueChangedHandler = ^(GCExtendedGamepad* pad, GCControllerElement*) {
      snapshot(pad);
    };
    dispatch_async(g_queue, ^{
      snapshot(gp);
    });
    std::fprintf(stderr, "[INPUT] controller connected: %s\n", name_of(c));
    return true;
}

void detach(GCController* c) {
    if (c != g_active) {
        return;
    }
    std::fprintf(stderr, "[INPUT] controller disconnected: %s\n", name_of(c));
    c.extendedGamepad.valueChangedHandler = nil;
    g_active = nil;
    dispatch_async(g_queue, ^{
      set_device_pad({});
    });
    for (GCController* other in GCController.controllers) {
        if (other != c && attach(other)) {
            return;
        }
    }
}

}

void start_gamepads() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    g_queue = dispatch_queue_create("MacAndreas.gamepad", DISPATCH_QUEUE_SERIAL);
    GCController.shouldMonitorBackgroundEvents = YES;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification* n) {
                      GCController* c = n.object;
                      if (g_active == nil) {
                          attach(c);
                      }
                    }];
    [center addObserverForName:GCControllerDidDisconnectNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification* n) {
                      detach(n.object);
                    }];
    for (GCController* c in GCController.controllers) {
        if (attach(c)) {
            break;
        }
    }
}

}
