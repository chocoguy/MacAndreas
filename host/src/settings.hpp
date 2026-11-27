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

enum class WindowMode { windowed, borderless, fullscreen };

struct Settings {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t refresh = 60;
    bool skip_intro = false;
    bool ignore_focus_loss = false;
    std::vector<uint32_t> watch;
    std::vector<std::vector<uint32_t>> watch_chains;
    struct CameraPin {
        uint64_t frame;
        bool restore;
        float position[3];
        float target[3];
    };
    std::vector<CameraPin> camera_pins;
    struct MemoryDump {
        uint64_t frame;
        uint32_t addr;
        uint32_t size;
        std::filesystem::path path;
    };
    std::vector<MemoryDump> memory_dumps;
    bool pool_stats = false;
    std::vector<uint64_t> pool_detail_frames;
    std::filesystem::path capture_dir;
    std::vector<uint32_t> capture_frames;
    bool capture_all = false;
    uint32_t exit_after_frames = 0;
    std::string keys;
    std::filesystem::path shader_dump_dir;
    std::vector<DisplayMode> modes;
    bool silent_audio = false;
    bool audio_clockless = false;
    std::filesystem::path audio_capture;
    WindowMode window_mode = WindowMode::windowed;
    uint32_t display = 0;
    bool mouse_capture = true;
    std::filesystem::path frame_times;
};

Settings& settings();
std::vector<DisplayMode> build_mode_list(DisplayMode configured, const std::vector<DisplayMode>& requested,
                                         DisplayMode display);
const std::vector<DisplayMode>& display_modes();
bool parse_mode(std::string_view text, DisplayMode& out);
bool parse_window_mode(std::string_view text, WindowMode& out);
std::string_view window_mode_name(WindowMode mode);

}
