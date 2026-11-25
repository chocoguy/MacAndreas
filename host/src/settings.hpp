#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ma::host {

struct DisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const DisplayMode&) const = default;
};

struct Settings {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t refresh = 60;
    bool skip_intro = false;
    std::filesystem::path capture_dir;
    std::vector<uint32_t> capture_frames;
    bool capture_all = false;
    uint32_t exit_after_frames = 0;
    std::string keys;
    std::vector<DisplayMode> modes;
};

Settings& settings();
std::vector<DisplayMode> build_mode_list(DisplayMode configured, const std::vector<DisplayMode>& requested,
                                         DisplayMode display);
const std::vector<DisplayMode>& display_modes();
bool parse_mode(std::string_view text, DisplayMode& out);

}
