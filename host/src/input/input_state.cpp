#include "input_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace ma::host::input {

namespace {

struct State {
    std::mutex mutex;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t dz = 0;
    std::array<bool, 8> buttons{};
    std::array<bool, 256> keys{};
    std::array<bool, 256> vks{};
    std::array<bool, 256> toggles{};
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    PadState device_pad;
    PadState script_pad;
};

constexpr uint8_t kMouseVks[] = {0x01, 0x02, 0x04, 0x05, 0x06};

State& state() {
    static State s;
    return s;
}

}

void add_mouse_motion(int32_t dx, int32_t dy) {
    std::lock_guard lock(state().mutex);
    state().dx += dx;
    state().dy += dy;
}

void add_mouse_wheel(int32_t dz) {
    std::lock_guard lock(state().mutex);
    state().dz += dz;
}

void set_mouse_button(unsigned button, bool down) {
    std::lock_guard lock(state().mutex);
    if (button < state().buttons.size()) {
        state().buttons[button] = down;
    }
    if (button < std::size(kMouseVks)) {
        state().vks[kMouseVks[button]] = down;
    }
}

void set_key(uint8_t dik, bool down) {
    std::lock_guard lock(state().mutex);
    state().keys[dik] = down;
}

void set_vk(uint8_t vk, bool down) {
    std::lock_guard lock(state().mutex);
    State& s = state();
    if (down && !s.vks[vk]) {
        s.toggles[vk] = !s.toggles[vk];
    }
    s.vks[vk] = down;
}

bool vk_down(uint8_t vk) {
    std::lock_guard lock(state().mutex);
    return state().vks[vk];
}

uint32_t key_state(uint8_t vk) {
    std::lock_guard lock(state().mutex);
    uint32_t value = state().toggles[vk] ? 1 : 0;
    if (state().vks[vk]) {
        value |= 0xffffff80;
    }
    return value;
}

void set_cursor(int32_t x, int32_t y) {
    std::lock_guard lock(state().mutex);
    state().cursor_x = x;
    state().cursor_y = y;
}

std::pair<int32_t, int32_t> cursor() {
    std::lock_guard lock(state().mutex);
    return {state().cursor_x, state().cursor_y};
}

bool mouse_button(unsigned button) {
    std::lock_guard lock(state().mutex);
    return button < state().buttons.size() && state().buttons[button];
}

void reset() {
    std::lock_guard lock(state().mutex);
    State& s = state();
    s.dx = s.dy = s.dz = 0;
    s.buttons.fill(false);
    s.keys.fill(false);
    s.vks.fill(false);
    s.script_pad = {};
}

void write_mouse_state(uint8_t* out, size_t size) {
    std::lock_guard lock(state().mutex);
    State& s = state();
    int32_t axes[3] = {s.dx, s.dy, s.dz};
    std::memcpy(out, axes, std::min<size_t>(size, sizeof(axes)));
    for (size_t i = 0; i < s.buttons.size() && 12 + i < size; ++i) {
        out[12 + i] = s.buttons[i] ? 0x80 : 0;
    }
    s.dx = s.dy = s.dz = 0;
}

void write_keyboard_state(uint8_t* out, size_t size) {
    std::lock_guard lock(state().mutex);
    for (size_t i = 0; i < std::min<size_t>(size, 256); ++i) {
        out[i] = state().keys[i] ? 0x80 : 0;
    }
}

namespace {

struct PadName {
    std::string_view name;
    PadButton button;
};

constexpr PadName kPadNames[] = {
    {"a", PadButton::south},     {"b", PadButton::east},       {"x", PadButton::west},       {"y", PadButton::north},
    {"lb", PadButton::lb},       {"rb", PadButton::rb},        {"lt", PadButton::lt},        {"rt", PadButton::rt},
    {"back", PadButton::back},   {"start", PadButton::start},  {"ls", PadButton::ls},        {"rs", PadButton::rs},
    {"up", PadButton::up},       {"down", PadButton::down},    {"left", PadButton::left},    {"right", PadButton::right},
};

constexpr PadButton kJoyButtons[kJoystickButtons] = {
    PadButton::east, PadButton::south, PadButton::west, PadButton::north, PadButton::lt,    PadButton::rt,
    PadButton::lb,   PadButton::rb,    PadButton::back, PadButton::ls,    PadButton::rs,    PadButton::start,
    PadButton::up,   PadButton::right, PadButton::down, PadButton::left,
};

int32_t scale_axis(float v, const AxisRange& r) {
    v = std::clamp(v, -1.0f, 1.0f);
    double span = static_cast<double>(r.max) - r.min;
    return static_cast<int32_t>(std::lround(r.min + (v + 1.0) * 0.5 * span));
}

void put32(uint8_t* out, size_t size, size_t offset, uint32_t v) {
    if (offset + 4 <= size) {
        std::memcpy(out + offset, &v, 4);
    }
}

}

std::optional<PadButton> pad_button_for_name(std::string_view name) {
    for (const PadName& p : kPadNames) {
        if (p.name == name) {
            return p.button;
        }
    }
    return std::nullopt;
}

void set_device_pad(const PadState& pad) {
    std::lock_guard lock(state().mutex);
    state().device_pad = pad;
}

void set_script_pad_button(PadButton button, bool down) {
    std::lock_guard lock(state().mutex);
    state().script_pad.set(button, down);
}

void set_script_pad_stick(unsigned stick, float x, float y) {
    std::lock_guard lock(state().mutex);
    PadState& p = state().script_pad;
    (stick == 0 ? p.lx : p.rx) = std::clamp(x, -1.0f, 1.0f);
    (stick == 0 ? p.ly : p.ry) = std::clamp(y, -1.0f, 1.0f);
}

PadState pad_state() {
    std::lock_guard lock(state().mutex);
    const PadState& d = state().device_pad;
    const PadState& s = state().script_pad;
    PadState out = d.connected ? d : PadState{};
    out.connected = d.connected;
    out.buttons |= s.buttons;
    auto pick = [](float device, float script) { return script != 0 ? script : device; };
    out.lx = pick(out.lx, s.lx);
    out.ly = pick(out.ly, s.ly);
    out.rx = pick(out.rx, s.rx);
    out.ry = pick(out.ry, s.ry);
    return out;
}

void write_joystick_state(uint8_t* out, size_t size, const PadState& pad, const AxisRange (&ranges)[kJoystickAxes]) {
    std::memset(out, 0, size);
    float axes[kJoystickAxes] = {pad.lx, -pad.ly, pad.rx, 0, 0, -pad.ry};
    for (size_t i = 0; i < kJoystickAxes; ++i) {
        put32(out, size, 4 * i, static_cast<uint32_t>(scale_axis(axes[i], ranges[i])));
    }
    for (size_t i = 0; i < 4; ++i) {
        put32(out, size, 32 + 4 * i, 0xffffffffu);
    }
    for (size_t i = 0; i < kJoystickButtons && 48 + i < size; ++i) {
        out[48 + i] = pad.pressed(kJoyButtons[i]) ? 0x80 : 0;
    }
}

}
