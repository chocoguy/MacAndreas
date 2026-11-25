#include "com_object.hpp"
#include "process.hpp"

#include "../input/input_state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kDiDevTypeMouse = 0x12;
constexpr uint32_t kDiDevTypeKeyboard = 0x13;
constexpr uint32_t kDiDevTypeJoystick = 0x14;
constexpr uint32_t kDidcAttached = 0x1;
constexpr uint32_t kDiErrNotAcquired = 0x8007000cu;
constexpr uint32_t kDiErrDeviceNotReg = 0x80040154u;

enum class Kind { mouse, keyboard, other };

struct Device : ComObject {
    Kind kind = Kind::other;
    uint32_t data_size = 0;
    bool acquired = false;
};

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
    default: return kDiDevTypeJoystick;
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
    mem::write<uint32_t>(caps + 12, d->kind == Kind::mouse ? 3 : 0);
    mem::write<uint32_t>(caps + 16, d->kind == Kind::mouse ? 5 : d->kind == Kind::keyboard ? 128 : 0);
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
    mem::write<uint32_t>(info + 36, dev_type(d->kind));
    const char* name = d->kind == Kind::mouse ? "Mouse" : d->kind == Kind::keyboard ? "Keyboard" : "Device";
    mem::write_cstr(info + 40, name);
    mem::write_cstr(info + 300, name);
    return kSOk;
}

ComInterface g_device("dinput8.dll", "IDirectInputDevice8A",
                      {
                          com_method<query_interface>("QueryInterface"),
                          com_method<com_obj_add_ref>("AddRef"),
                          com_method<com_obj_release>("Release"),
                          com_method<dev_get_capabilities>("GetCapabilities"),
                          {"EnumObjects", 4, com_noop},
                          {"GetProperty", 3, nullptr, kENotImpl},
                          {"SetProperty", 3, com_noop},
                          com_method<dev_acquire>("Acquire"),
                          com_method<dev_unacquire>("Unacquire"),
                          com_method<dev_get_device_state>("GetDeviceState"),
                          com_method<dev_get_device_data>("GetDeviceData"),
                          com_method<dev_set_data_format>("SetDataFormat"),
                          {"SetEventNotification", 2, com_noop},
                          {"SetCooperativeLevel", 3, com_noop},
                          {"GetObjectInfo", 4, nullptr, kENotImpl},
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
                          {"Poll", 1, nullptr, kSFalse},
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
    } else {
        std::fprintf(stderr, "[DINPUT] CreateDevice(%s): no such device\n", guid_string(guid).c_str());
        return kDiErrDeviceNotReg;
    }
    std::fprintf(stderr, "[DINPUT] CreateDevice(%s)\n", device->kind == Kind::mouse ? "mouse" : "keyboard");
    com_out(out, com_objects().wrap(g_device, std::move(device)));
    return kSOk;
}

uint32_t di_enum_devices(uint32_t self, uint32_t type, uint32_t callback, uint32_t ref, uint32_t flags) {
    return kSOk;
}

uint32_t di_get_device_status(uint32_t self, uint32_t guid) {
    return is_sys_mouse(guid) || is_sys_keyboard(guid) ? kSOk : kDiErrDeviceNotReg;
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
    com_out(out, com_objects().wrap(g_dinput, std::make_unique<DirectInput>()));
    return kSOk;
}

constexpr ApiDef kDinput8[] = {
    native_export<Conv::stdcall, direct_input8_create>("dinput8.dll", "DirectInput8Create"),
};

}

std::span<const ApiDef> dinput8_apis() { return kDinput8; }

}
