#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ma::host {

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
};

Settings& settings();

}
