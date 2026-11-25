#pragma once

#include <cstdint>
#include <cstddef>

namespace ma::host::input {

void add_mouse_motion(int32_t dx, int32_t dy);
void add_mouse_wheel(int32_t dz);
void set_mouse_button(unsigned button, bool down);
void set_key(uint8_t dik, bool down);
void set_vk(uint8_t vk, bool down);
bool vk_down(uint8_t vk);
uint32_t key_state(uint8_t vk);
void reset();

void write_mouse_state(uint8_t* out, size_t size);
void write_keyboard_state(uint8_t* out, size_t size);

}
