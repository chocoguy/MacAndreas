#include "settings.hpp"

#include "platform/window.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ma::host {

namespace {

constexpr DisplayMode kStandardModes[] = {
    {1280, 720},  {1280, 800},  {1366, 768},  {1440, 900},  {1600, 900},  {1680, 1050},
    {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}, {3840, 2160},
};

}

Settings& settings() {
    static Settings s;
    return s;
}

bool parse_window_mode(std::string_view text, WindowMode& out) {
    if (text == "windowed") {
        out = WindowMode::windowed;
    } else if (text == "borderless") {
        out = WindowMode::borderless;
    } else if (text == "fullscreen") {
        out = WindowMode::fullscreen;
    } else {
        return false;
    }
    return true;
}

std::string_view window_mode_name(WindowMode mode) {
    switch (mode) {
    case WindowMode::windowed: return "windowed";
    case WindowMode::borderless: return "borderless";
    case WindowMode::fullscreen: return "fullscreen";
    }
    return "windowed";
}

bool parse_mode(std::string_view text, DisplayMode& out) {
    unsigned w = 0;
    unsigned h = 0;
    char tail = 0;
    if (std::sscanf(std::string(text).c_str(), "%ux%u%c", &w, &h, &tail) != 2 || w < 640 || h < 480 || w > 8192 ||
        h > 8192) {
        return false;
    }
    out = {w, h};
    return true;
}

std::vector<DisplayMode> build_mode_list(DisplayMode configured, const std::vector<DisplayMode>& requested,
                                         DisplayMode display) {
    std::vector<DisplayMode> rest;
    if (!requested.empty()) {
        rest = requested;
    } else {
        for (const DisplayMode& m : kStandardModes) {
            if (display.width == 0 || (m.width <= display.width && m.height <= display.height)) {
                rest.push_back(m);
            }
        }
        if (display.width != 0) {
            rest.push_back(display);
        }
    }
    std::ranges::sort(rest, [](const DisplayMode& a, const DisplayMode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    std::vector<DisplayMode> out{configured};
    for (const DisplayMode& m : rest) {
        if (std::ranges::find(out, m) == out.end()) {
            out.push_back(m);
        }
    }
    return out;
}

const std::vector<DisplayMode>& display_modes() {
    static const std::vector<DisplayMode> modes =
        build_mode_list({settings().width, settings().height}, settings().modes,
                        platform::display_pixels(settings().display));
    return modes;
}

}
