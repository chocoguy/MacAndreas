#include "input_state.hpp"

#include <algorithm>
#include <array>
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

}
