#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ma::host::win32 {

struct Window {
    uint32_t hwnd = 0;
    std::string class_name;
    std::string title;
    uint32_t wndproc = 0;
    uint32_t instance = 0;
    uint32_t style = 0;
    uint32_t ex_style = 0;
    uint32_t parent = 0;
    uint32_t user_data = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool visible = false;
    std::map<int32_t, uint32_t> extra;
};

std::pair<uint32_t, uint32_t> screen_size();
std::vector<Window> windows_snapshot();
uint32_t map_virtual_key(uint32_t code, uint32_t type);

}
