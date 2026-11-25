#pragma once

#include "../input/keys.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace ma::host::platform {

using PostFn = std::function<void(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)>;

using KeyFn = std::function<void(uint32_t hwnd, const input::Key& key, bool down, bool repeat)>;

void set_post_callback(PostFn post);
void set_key_callback(KeyFn key);
void create_window(uint32_t hwnd, const std::string& title, uint32_t width, uint32_t height);
void destroy_window(uint32_t hwnd);
void show_window(uint32_t hwnd, bool show);
void set_title(uint32_t hwnd, const std::string& title);
void set_client_pixels(uint32_t hwnd, uint32_t width, uint32_t height);
void* metal_layer(uint32_t hwnd);
void pump_events();
bool has_window_server();

}
