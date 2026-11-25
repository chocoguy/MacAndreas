#include "keys.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>

namespace ma::host::input {

namespace {

constexpr Key kKeys[] = {
    {"a", 0x00, 'A', 0, 0x1e, false},
    {"s", 0x01, 'S', 0, 0x1f, false},
    {"d", 0x02, 'D', 0, 0x20, false},
    {"f", 0x03, 'F', 0, 0x21, false},
    {"h", 0x04, 'H', 0, 0x23, false},
    {"g", 0x05, 'G', 0, 0x22, false},
    {"z", 0x06, 'Z', 0, 0x2c, false},
    {"x", 0x07, 'X', 0, 0x2d, false},
    {"c", 0x08, 'C', 0, 0x2e, false},
    {"v", 0x09, 'V', 0, 0x2f, false},
    {"b", 0x0b, 'B', 0, 0x30, false},
    {"q", 0x0c, 'Q', 0, 0x10, false},
    {"w", 0x0d, 'W', 0, 0x11, false},
    {"e", 0x0e, 'E', 0, 0x12, false},
    {"r", 0x0f, 'R', 0, 0x13, false},
    {"y", 0x10, 'Y', 0, 0x15, false},
    {"t", 0x11, 'T', 0, 0x14, false},
    {"1", 0x12, '1', 0, 0x02, false},
    {"2", 0x13, '2', 0, 0x03, false},
    {"3", 0x14, '3', 0, 0x04, false},
    {"4", 0x15, '4', 0, 0x05, false},
    {"6", 0x16, '6', 0, 0x07, false},
    {"5", 0x17, '5', 0, 0x06, false},
    {"equals", 0x18, 0xbb, 0, 0x0d, false},
    {"9", 0x19, '9', 0, 0x0a, false},
    {"7", 0x1a, '7', 0, 0x08, false},
    {"minus", 0x1b, 0xbd, 0, 0x0c, false},
    {"8", 0x1c, '8', 0, 0x09, false},
    {"0", 0x1d, '0', 0, 0x0b, false},
    {"rbracket", 0x1e, 0xdd, 0, 0x1b, false},
    {"o", 0x1f, 'O', 0, 0x18, false},
    {"u", 0x20, 'U', 0, 0x16, false},
    {"lbracket", 0x21, 0xdb, 0, 0x1a, false},
    {"i", 0x22, 'I', 0, 0x17, false},
    {"p", 0x23, 'P', 0, 0x19, false},
    {"return", 0x24, 0x0d, 0, 0x1c, false},
    {"l", 0x25, 'L', 0, 0x26, false},
    {"j", 0x26, 'J', 0, 0x24, false},
    {"apostrophe", 0x27, 0xde, 0, 0x28, false},
    {"k", 0x28, 'K', 0, 0x25, false},
    {"semicolon", 0x29, 0xba, 0, 0x27, false},
    {"backslash", 0x2a, 0xdc, 0, 0x2b, false},
    {"comma", 0x2b, 0xbc, 0, 0x33, false},
    {"slash", 0x2c, 0xbf, 0, 0x35, false},
    {"n", 0x2d, 'N', 0, 0x31, false},
    {"m", 0x2e, 'M', 0, 0x32, false},
    {"period", 0x2f, 0xbe, 0, 0x34, false},
    {"tab", 0x30, 0x09, 0, 0x0f, false},
    {"space", 0x31, 0x20, 0, 0x39, false},
    {"grave", 0x32, 0xc0, 0, 0x29, false},
    {"backspace", 0x33, 0x08, 0, 0x0e, false},
    {"escape", 0x35, 0x1b, 0, 0x01, false},
    {"lshift", 0x38, kVkShift, kVkLShift, 0x2a, false},
    {"capslock", 0x39, kVkCapital, 0, 0x3a, false},
    {"lalt", 0x3a, kVkMenu, kVkLMenu, 0x38, false},
    {"lctrl", 0x3b, kVkControl, kVkLControl, 0x1d, false},
    {"rshift", 0x3c, kVkShift, kVkRShift, 0x36, false},
    {"ralt", 0x3d, kVkMenu, kVkRMenu, 0x38, true},
    {"rctrl", 0x3e, kVkControl, kVkRControl, 0x1d, true},
    {"numdecimal", 0x41, 0x6e, 0, 0x53, false},
    {"nummultiply", 0x43, 0x6a, 0, 0x37, false},
    {"numplus", 0x45, 0x6b, 0, 0x4e, false},
    {"numdivide", 0x4b, 0x6f, 0, 0x35, true},
    {"numenter", 0x4c, 0x0d, 0, 0x1c, true},
    {"numminus", 0x4e, 0x6d, 0, 0x4a, false},
    {"num0", 0x52, 0x60, 0, 0x52, false},
    {"num1", 0x53, 0x61, 0, 0x4f, false},
    {"num2", 0x54, 0x62, 0, 0x50, false},
    {"num3", 0x55, 0x63, 0, 0x51, false},
    {"num4", 0x56, 0x64, 0, 0x4b, false},
    {"num5", 0x57, 0x65, 0, 0x4c, false},
    {"num6", 0x58, 0x66, 0, 0x4d, false},
    {"num7", 0x59, 0x67, 0, 0x47, false},
    {"num8", 0x5b, 0x68, 0, 0x48, false},
    {"num9", 0x5c, 0x69, 0, 0x49, false},
    {"f5", 0x60, 0x74, 0, 0x3f, false},
    {"f6", 0x61, 0x75, 0, 0x40, false},
    {"f7", 0x62, 0x76, 0, 0x41, false},
    {"f3", 0x63, 0x72, 0, 0x3d, false},
    {"f8", 0x64, 0x77, 0, 0x42, false},
    {"f9", 0x65, 0x78, 0, 0x43, false},
    {"f11", 0x67, 0x7a, 0, 0x57, false},
    {"f13", 0x69, 0x7c, 0, 0x64, false},
    {"f14", 0x6b, 0x7d, 0, 0x65, false},
    {"f10", 0x6d, kVkF10, 0, 0x44, false},
    {"f12", 0x6f, 0x7b, 0, 0x58, false},
    {"f15", 0x71, 0x7e, 0, 0x66, false},
    {"insert", 0x72, 0x2d, 0, 0x52, true},
    {"home", 0x73, 0x24, 0, 0x47, true},
    {"pageup", 0x74, 0x21, 0, 0x49, true},
    {"delete", 0x75, 0x2e, 0, 0x53, true},
    {"f4", 0x76, 0x73, 0, 0x3e, false},
    {"end", 0x77, 0x23, 0, 0x4f, true},
    {"f2", 0x78, 0x71, 0, 0x3c, false},
    {"pagedown", 0x79, 0x22, 0, 0x51, true},
    {"f1", 0x7a, 0x70, 0, 0x3b, false},
    {"left", 0x7b, 0x25, 0, 0x4b, true},
    {"right", 0x7c, 0x27, 0, 0x4d, true},
    {"down", 0x7d, 0x28, 0, 0x50, true},
    {"up", 0x7e, 0x26, 0, 0x48, true},
};

bool same_name(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

struct Release {
    const Key* key;
    uint64_t idle;
};

struct Script {
    std::mutex mutex;
    std::vector<KeyStep> steps;
    size_t next_game = 0;
    size_t next_movie = 0;
    uint64_t idle = 0;
    std::vector<Release> releases;
    KeySink sink;
};

Script& script() {
    static Script s;
    return s;
}

}

const Key* key_for_mac(uint16_t mac) {
    for (const Key& k : kKeys) {
        if (k.mac == mac) {
            return &k;
        }
    }
    return nullptr;
}

const Key* key_for_name(std::string_view name) {
    if (same_name(name, "enter")) {
        name = "return";
    } else if (same_name(name, "esc")) {
        name = "escape";
    }
    for (const Key& k : kKeys) {
        if (same_name(k.name, name)) {
            return &k;
        }
    }
    return nullptr;
}

uint8_t dik(const Key& key) { return static_cast<uint8_t>(key.scan | (key.extended ? 0x80 : 0)); }

uint8_t other_side(uint8_t side_vk) {
    if (side_vk < kVkLShift || side_vk > kVkRMenu) {
        return 0;
    }
    return static_cast<uint8_t>(side_vk ^ 1);
}

std::vector<KeyStep> parse_key_script(std::string_view text, std::string& error) {
    std::vector<KeyStep> steps;
    while (!text.empty()) {
        auto comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        auto colon = item.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            error = "expected FRAME:KEY in \"" + std::string(item) + "\"";
            return {};
        }
        std::string_view when = item.substr(0, colon);
        std::string_view name = item.substr(colon + 1);
        KeyStep step;
        if (when.back() == 'm') {
            step.movie = true;
            when.remove_suffix(1);
        }
        auto [end, ec] = std::from_chars(when.data(), when.data() + when.size(), step.frame);
        if (ec != std::errc{} || end != when.data() + when.size()) {
            error = "bad frame number in \"" + std::string(item) + "\"";
            return {};
        }
        int mode = 0;
        if (!name.empty() && (name.back() == '+' || name.back() == '-')) {
            mode = name.back() == '+' ? 1 : -1;
            name.remove_suffix(1);
        }
        step.key = key_for_name(name);
        if (!step.key) {
            error = "unknown key \"" + std::string(name) + "\"";
            return {};
        }
        step.down = mode >= 0;
        step.tap = mode == 0;
        steps.push_back(step);
    }
    std::ranges::stable_sort(steps, [](const KeyStep& a, const KeyStep& b) { return a.frame < b.frame; });
    return steps;
}

void set_key_script(std::vector<KeyStep> steps) {
    std::lock_guard lock(script().mutex);
    script().steps = std::move(steps);
    script().next_game = 0;
    script().next_movie = 0;
    script().releases.clear();
}

void note_idle_peek() {
    std::lock_guard lock(script().mutex);
    ++script().idle;
}

void set_key_sink(KeySink sink) {
    std::lock_guard lock(script().mutex);
    script().sink = std::move(sink);
}

void poll_key_script() {
    std::vector<const Key*> up;
    KeySink sink;
    {
        std::lock_guard lock(script().mutex);
        Script& s = script();
        if (s.releases.empty()) {
            return;
        }
        std::erase_if(s.releases, [&](const Release& r) {
            if (s.idle <= r.idle) {
                return false;
            }
            up.push_back(r.key);
            return true;
        });
        sink = s.sink;
    }
    if (sink) {
        for (const Key* k : up) {
            sink(*k, false);
        }
    }
}

void run_key_script(bool movie, uint64_t frame) {
    std::vector<KeyStep> due;
    KeySink sink;
    {
        std::lock_guard lock(script().mutex);
        Script& s = script();
        size_t& next = movie ? s.next_movie : s.next_game;
        while (next < s.steps.size()) {
            const KeyStep& step = s.steps[next];
            if (step.movie != movie) {
                ++next;
                continue;
            }
            if (step.frame > frame) {
                break;
            }
            due.push_back(step);
            if (step.tap) {
                s.releases.push_back(Release{step.key, s.idle});
            }
            ++next;
        }
        sink = s.sink;
    }
    if (sink) {
        for (const KeyStep& step : due) {
            sink(*step.key, step.down);
        }
    }
}

}
