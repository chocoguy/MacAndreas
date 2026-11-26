#include "com_object.hpp"
#include "process.hpp"

#include "../abi.hpp"
#include "../thread.hpp"

#include "user32.hpp"

#include "../input/gamepad.hpp"
#include "../input/input_state.hpp"
#include "../input/keys.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string>
#include <cstdio>
#include <cstring>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kDiDevTypeMouse = 0x12;
constexpr uint32_t kDiDevTypeKeyboard = 0x13;
constexpr uint32_t kDiDevTypeJoystick = 0x14;
constexpr uint32_t kDiDevTypeGamepad = 0x10215;
constexpr uint32_t kDidcAttached = 0x1;
constexpr uint32_t kDiErrNotAcquired = 0x8007000cu;
constexpr uint32_t kDiErrDeviceNotReg = 0x80040154u;
constexpr uint32_t kDiErrObjectNotFound = 0x80070002u;
constexpr uint32_t kDiPropRange = 4;
constexpr uint32_t kDiPropVidPid = 24;
constexpr uint32_t kDiphDevice = 0;
constexpr uint32_t kDiphByOffset = 1;
constexpr uint32_t kDiClassPointer = 2;
constexpr uint32_t kDiClassKeyboard = 3;
constexpr uint32_t kDiClassGameCtrl = 4;
constexpr uint32_t kDiedflForceFeedback = 0x100;
constexpr uint32_t kDidftAbsAxis = 0x2;
constexpr uint32_t kDidftPshButton = 0x4;
constexpr uint32_t kJoyAxisOffsets[] = {0, 4, 8, 20};
constexpr uint32_t kJoyButtonOffset = 48;
constexpr uint32_t kInstanceSize = 580;

enum class Kind { mouse, keyboard, joystick, other };

struct Device : ComObject {
    Kind kind = Kind::other;
    uint32_t data_size = 0;
    bool acquired = false;
    input::AxisRange ranges[input::kJoystickAxes];
};

void write_guid(uint32_t at, uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4) {
    mem::write<uint32_t>(at, d1);
    mem::write<uint16_t>(at + 4, d2);
    mem::write<uint16_t>(at + 6, d3);
    uint32_t i = 0;
    for (uint8_t b : d4) {
        mem::write<uint8_t>(at + 8 + i++, b);
    }
}

void write_joystick_guid(uint32_t at) {
    write_guid(at, 0x4d41434a, 0x0001, 0x11f0, {0x80, 0x00, 0x4d, 0x61, 0x63, 0x41, 0x6e, 0x64});
}

bool is_joystick(uint32_t g) {
    return guid_equals(g, 0x4d41434a, 0x0001, 0x11f0, {0x80, 0x00, 0x4d, 0x61, 0x63, 0x41, 0x6e, 0x64});
}

void write_axis_guid(uint32_t at, uint32_t d1) {
    write_guid(at, d1, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
}

int axis_index(uint32_t offset) {
    switch (offset) {
    case 0: return 0;
    case 4: return 1;
    case 8: return 2;
    case 20: return 5;
    default: return -1;
    }
}

struct DirectInput : ComObject {};

bool is_sys_mouse(uint32_t g) {
    return guid_equals(g, 0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
}

bool is_sys_keyboard(uint32_t g) {
    return guid_equals(g, 0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
}

uint32_t query_interface(uint32_t self, uint32_t riid, uint32_t out) {
    com_out(out, 0);
    return kENoInterface;
}

uint32_t dev_type(Kind k) {
    switch (k) {
    case Kind::mouse: return kDiDevTypeMouse;
    case Kind::keyboard: return kDiDevTypeKeyboard;
    case Kind::joystick: return kDiDevTypeGamepad;
    default: return kDiDevTypeJoystick;
    }
}

const char* dev_name(Kind k) {
    switch (k) {
    case Kind::mouse: return "Mouse";
    case Kind::keyboard: return "Keyboard";
    case Kind::joystick: return "Controller";
    default: return "Device";
    }
}

uint32_t dev_get_capabilities(uint32_t self, uint32_t caps) {
    auto* d = com_get<Device>(self);
    if (!d || caps == 0) {
        return kEInvalidArg;
    }
    uint32_t size = mem::read<uint32_t>(caps);
    std::memset(mem::ptr(caps + 4), 0, size > 4 ? size - 4 : 0);
    mem::write<uint32_t>(caps + 4, kDidcAttached);
    mem::write<uint32_t>(caps + 8, dev_type(d->kind));
    uint32_t axes = d->kind == Kind::mouse ? 3 : d->kind == Kind::joystick ? std::size(kJoyAxisOffsets) : 0;
    uint32_t buttons = d->kind == Kind::mouse      ? 5
                       : d->kind == Kind::keyboard ? 128
                       : d->kind == Kind::joystick ? input::kJoystickButtons
                                                   : 0;
    mem::write<uint32_t>(caps + 12, axes);
    mem::write<uint32_t>(caps + 16, buttons);
    return kSOk;
}

uint32_t dev_acquire(uint32_t self) {
    if (auto* d = com_get<Device>(self)) {
        d->acquired = true;
    }
    return kSOk;
}

uint32_t dev_unacquire(uint32_t self) {
    if (auto* d = com_get<Device>(self)) {
        d->acquired = false;
    }
    return kSOk;
}

uint32_t dev_get_device_state(uint32_t self, uint32_t size, uint32_t data) {
    auto* d = com_get<Device>(self);
    if (!d || data == 0) {
        return kEInvalidArg;
    }
    if (!d->acquired) {
        return kDiErrNotAcquired;
    }
    std::memset(mem::ptr(data), 0, size);
    if (d->kind == Kind::mouse) {
        input::write_mouse_state(mem::ptr(data), size);
    } else if (d->kind == Kind::keyboard) {
        input::write_keyboard_state(mem::ptr(data), size);
    } else if (d->kind == Kind::joystick) {
        input::PadState pad = input::pad_state();
        input::write_joystick_state(mem::ptr(data), size, pad, d->ranges);
        static bool start_down = false;
        if (pad.pressed(input::PadButton::start) != start_down) {
            start_down = !start_down;
            key_event(0, *input::key_for_name("escape"), start_down, false);
        }
    }
    return kSOk;
}

uint32_t dev_poll(uint32_t self) {
    auto* d = com_get<Device>(self);
    if (!d) {
        return kEInvalidArg;
    }
    if (d->kind != Kind::joystick) {
        return kSFalse;
    }
    return d->acquired ? kSOk : kDiErrNotAcquired;
}

uint32_t dev_set_property(uint32_t self, uint32_t prop, uint32_t header) {
    auto* d = com_get<Device>(self);
    if (!d || header == 0) {
        return kEInvalidArg;
    }
    if (prop != kDiPropRange || d->kind != Kind::joystick) {
        return kSOk;
    }
    uint32_t obj = mem::read<uint32_t>(header + 8);
    uint32_t how = mem::read<uint32_t>(header + 12);
    input::AxisRange r{static_cast<int32_t>(mem::read<uint32_t>(header + 16)),
                       static_cast<int32_t>(mem::read<uint32_t>(header + 20))};
    if (r.min > r.max) {
        return kEInvalidArg;
    }
    if (how == kDiphDevice) {
        for (auto& range : d->ranges) {
            range = r;
        }
        return kSOk;
    }
    int axis = how == kDiphByOffset ? axis_index(obj) : -1;
    if (axis < 0) {
        return kDiErrObjectNotFound;
    }
    d->ranges[axis] = r;
    return kSOk;
}

uint32_t dev_get_property(uint32_t self, uint32_t prop, uint32_t header) {
    auto* d = com_get<Device>(self);
    if (!d || header == 0) {
        return kEInvalidArg;
    }
    if (d->kind != Kind::joystick) {
        return kENotImpl;
    }
    if (prop == kDiPropVidPid) {
        mem::write<uint32_t>(header + 16, 0);
        return kSOk;
    }
    if (prop == kDiPropRange) {
        uint32_t how = mem::read<uint32_t>(header + 12);
        int axis = how == kDiphByOffset ? axis_index(mem::read<uint32_t>(header + 8)) : 0;
        if (axis < 0) {
            return kDiErrObjectNotFound;
        }
        mem::write<uint32_t>(header + 16, static_cast<uint32_t>(d->ranges[axis].min));
        mem::write<uint32_t>(header + 20, static_cast<uint32_t>(d->ranges[axis].max));
        return kSOk;
    }
    return kENotImpl;
}

uint32_t dev_get_object_info(uint32_t self, uint32_t info, uint32_t obj, uint32_t how) {
    static constexpr const char* kAxisNames[] = {"X Axis", "Y Axis", "Z Axis", "", "", "Z Rotation"};
    static constexpr uint32_t kAxisGuids[] = {0xa36d02e0, 0xa36d02e1, 0xa36d02e2, 0, 0, 0xa36d02e3};
    auto* d = com_get<Device>(self);
    if (!d || info == 0) {
        return kEInvalidArg;
    }
    if (d->kind != Kind::joystick || how != kDiphByOffset) {
        return kDiErrObjectNotFound;
    }
    uint32_t size = mem::read<uint32_t>(info);
    int axis = axis_index(obj);
    bool button = obj >= kJoyButtonOffset && obj < kJoyButtonOffset + input::kJoystickButtons;
    if (axis < 0 && !button) {
        return kDiErrObjectNotFound;
    }
    std::memset(mem::ptr(info + 4), 0, size > 4 ? size - 4 : 0);
    mem::write<uint32_t>(info + 20, obj);
    if (axis >= 0) {
        write_axis_guid(info + 4, kAxisGuids[axis]);
        uint32_t instance = axis == 5 ? 3 : static_cast<uint32_t>(axis);
        mem::write<uint32_t>(info + 24, kDidftAbsAxis | instance << 8);
        mem::write_cstr(info + 32, kAxisNames[axis]);
    } else {
        uint32_t n = obj - kJoyButtonOffset;
        write_axis_guid(info + 4, 0xa36d02f0);
        mem::write<uint32_t>(info + 24, kDidftPshButton | n << 8);
        mem::write_cstr(info + 32, "Button " + std::to_string(n + 1));
    }
    return kSOk;
}

uint32_t dev_get_device_data(uint32_t self, uint32_t object_size, uint32_t data, uint32_t in_out, uint32_t flags) {
    if (in_out != 0) {
        mem::write<uint32_t>(in_out, 0);
    }
    return kSOk;
}

uint32_t dev_set_data_format(uint32_t self, uint32_t format) {
    auto* d = com_get<Device>(self);
    if (!d || format == 0) {
        return kEInvalidArg;
    }
    d->data_size = mem::read<uint32_t>(format + 12);
    return kSOk;
}

uint32_t dev_get_device_info(uint32_t self, uint32_t info) {
    auto* d = com_get<Device>(self);
    if (!d || info == 0) {
        return kEInvalidArg;
    }
    uint32_t size = mem::read<uint32_t>(info);
    std::memset(mem::ptr(info + 4), 0, size > 4 ? size - 4 : 0);
    if (d->kind == Kind::joystick) {
        write_joystick_guid(info + 4);
    }
    mem::write<uint32_t>(info + 36, dev_type(d->kind));
    mem::write_cstr(info + 40, dev_name(d->kind));
    mem::write_cstr(info + 300, dev_name(d->kind));
    return kSOk;
}

ComInterface g_device("dinput8.dll", "IDirectInputDevice8A",
                      {
                          com_method<query_interface>("QueryInterface"),
                          com_method<com_obj_add_ref>("AddRef"),
                          com_method<com_obj_release>("Release"),
                          com_method<dev_get_capabilities>("GetCapabilities"),
                          {"EnumObjects", 4, com_noop},
                          com_method<dev_get_property>("GetProperty"),
                          com_method<dev_set_property>("SetProperty"),
                          com_method<dev_acquire>("Acquire"),
                          com_method<dev_unacquire>("Unacquire"),
                          com_method<dev_get_device_state>("GetDeviceState"),
                          com_method<dev_get_device_data>("GetDeviceData"),
                          com_method<dev_set_data_format>("SetDataFormat"),
                          {"SetEventNotification", 2, com_noop},
                          {"SetCooperativeLevel", 3, com_noop},
                          com_method<dev_get_object_info>("GetObjectInfo"),
                          com_method<dev_get_device_info>("GetDeviceInfo"),
                          {"RunControlPanel", 3, com_noop},
                          {"Initialize", 4, com_noop},
                          {"CreateEffect", 5, nullptr, kENotImpl},
                          {"EnumEffects", 4, com_noop},
                          {"GetEffectInfo", 3, nullptr, kENotImpl},
                          {"GetForceFeedbackState", 2, nullptr, kENotImpl},
                          {"SendForceFeedbackCommand", 2, nullptr, kENotImpl},
                          {"EnumCreatedEffectObjects", 4, com_noop},
                          {"Escape", 2, nullptr, kENotImpl},
                          com_method<dev_poll>("Poll"),
                          {"SendDeviceData", 5, nullptr, kENotImpl},
                          {"EnumEffectsInFile", 5, nullptr, kENotImpl},
                          {"WriteEffectToFile", 5, nullptr, kENotImpl},
                          {"BuildActionMap", 4, nullptr, kENotImpl},
                          {"SetActionMap", 4, nullptr, kENotImpl},
                          {"GetImageInfo", 2, nullptr, kENotImpl},
                      });

uint32_t di_create_device(uint32_t self, uint32_t guid, uint32_t out, uint32_t outer) {
    com_out(out, 0);
    auto device = std::make_unique<Device>();
    if (is_sys_mouse(guid)) {
        device->kind = Kind::mouse;
    } else if (is_sys_keyboard(guid)) {
        device->kind = Kind::keyboard;
    } else if (is_joystick(guid)) {
        device->kind = Kind::joystick;
    } else {
        std::fprintf(stderr, "[DINPUT] CreateDevice(%s): no such device\n", guid_string(guid).c_str());
        return kDiErrDeviceNotReg;
    }
    std::fprintf(stderr, "[DINPUT] CreateDevice(%s)\n", dev_name(device->kind));
    com_out(out, com_objects().wrap(g_device, std::move(device)));
    return kSOk;
}

uint32_t di_enum_devices(uint32_t self, uint32_t type, uint32_t callback, uint32_t ref, uint32_t flags) {
    if (callback == 0) {
        return kEInvalidArg;
    }
    if (flags & kDiedflForceFeedback) {
        return kSOk;
    }
    struct Entry {
        Kind kind;
        uint32_t dev_class;
    };
    constexpr Entry kEntries[] = {{Kind::mouse, kDiClassPointer},
                                  {Kind::keyboard, kDiClassKeyboard},
                                  {Kind::joystick, kDiClassGameCtrl}};
    uint32_t inst = allocator().alloc(kInstanceSize);
    for (const Entry& e : kEntries) {
        uint32_t devtype = dev_type(e.kind);
        if (type != 0 && type != e.dev_class && type != (devtype & 0xff)) {
            continue;
        }
        std::memset(mem::ptr(inst), 0, kInstanceSize);
        mem::write<uint32_t>(inst, kInstanceSize);
        if (e.kind == Kind::mouse) {
            write_guid(inst + 4, 0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
        } else if (e.kind == Kind::keyboard) {
            write_guid(inst + 4, 0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
        } else {
            write_joystick_guid(inst + 4);
            write_guid(inst + 20, 0, 0, 0, {0, 0, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44});
            mem::write<uint16_t>(inst + 576, 1);
            mem::write<uint16_t>(inst + 578, 5);
        }
        if (e.kind != Kind::joystick) {
            std::memcpy(mem::ptr(inst + 20), mem::ptr(inst + 4), 16);
        }
        mem::write<uint32_t>(inst + 36, devtype);
        mem::write_cstr(inst + 40, dev_name(e.kind));
        mem::write_cstr(inst + 300, dev_name(e.kind));
        uint32_t more = guest_call<Conv::stdcall, uint32_t>(current_thread()->state, callback, inst, ref);
        if (more == 0) {
            break;
        }
    }
    allocator().free(inst);
    return kSOk;
}

uint32_t di_get_device_status(uint32_t self, uint32_t guid) {
    return is_sys_mouse(guid) || is_sys_keyboard(guid) || is_joystick(guid) ? kSOk : kDiErrDeviceNotReg;
}

ComInterface g_dinput("dinput8.dll", "IDirectInput8A",
                      {
                          com_method<query_interface>("QueryInterface"),
                          com_method<com_obj_add_ref>("AddRef"),
                          com_method<com_obj_release>("Release"),
                          com_method<di_create_device>("CreateDevice"),
                          com_method<di_enum_devices>("EnumDevices"),
                          com_method<di_get_device_status>("GetDeviceStatus"),
                          {"RunControlPanel", 3, com_noop},
                          {"Initialize", 3, com_noop},
                          {"FindDevice", 4, nullptr, kDiErrDeviceNotReg},
                          {"EnumDevicesBySemantics", 6, com_noop},
                          {"ConfigureDevices", 5, nullptr, kENotImpl},
                      });

uint32_t direct_input8_create(uint32_t instance, uint32_t version, uint32_t riid, uint32_t out, uint32_t outer) {
    if (out == 0) {
        return kEInvalidArg;
    }
    std::fprintf(stderr, "[DINPUT] DirectInput8Create(version=0x%04x)\n", version);
    input::start_gamepads();
    com_out(out, com_objects().wrap(g_dinput, std::make_unique<DirectInput>()));
    return kSOk;
}

constexpr ApiDef kDinput8[] = {
    native_export<Conv::stdcall, direct_input8_create>("dinput8.dll", "DirectInput8Create"),
};

}

std::span<const ApiDef> dinput8_apis() { return kDinput8; }

}
