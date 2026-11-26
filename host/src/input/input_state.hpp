#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace ma::host::input {

void add_mouse_motion(int32_t dx, int32_t dy);
void add_mouse_wheel(int32_t dz);
void set_mouse_button(unsigned button, bool down);
void set_key(uint8_t dik, bool down);
void set_vk(uint8_t vk, bool down);
bool vk_down(uint8_t vk);
uint32_t key_state(uint8_t vk);
void set_cursor(int32_t x, int32_t y);
std::pair<int32_t, int32_t> cursor();
bool mouse_button(unsigned button);
void reset();

void write_mouse_state(uint8_t* out, size_t size);
void write_keyboard_state(uint8_t* out, size_t size);

enum class PadButton : uint8_t { south, east, west, north, lb, rb, lt, rt, back, start, ls, rs, up, down, left, right, count };

struct PadState {
    bool connected = false;
    float lx = 0;
    float ly = 0;
    float rx = 0;
    float ry = 0;
    uint32_t buttons = 0;

    bool pressed(PadButton b) const { return (buttons >> static_cast<unsigned>(b)) & 1; }
    void set(PadButton b, bool down) {
        uint32_t bit = 1u << static_cast<unsigned>(b);
        buttons = down ? (buttons | bit) : (buttons & ~bit);
    }
};

struct AxisRange {
    int32_t min = 0;
    int32_t max = 0xffff;
};

constexpr size_t kJoystickAxes = 6;
constexpr size_t kJoystickButtons = 16;
constexpr size_t kJoyState2Size = 272;

std::optional<PadButton> pad_button_for_name(std::string_view name);
void set_device_pad(const PadState& state);
void set_script_pad_button(PadButton button, bool down);
void set_script_pad_stick(unsigned stick, float x, float y);
PadState pad_state();
void write_joystick_state(uint8_t* out, size_t size, const PadState& pad, const AxisRange (&ranges)[kJoystickAxes]);

}
