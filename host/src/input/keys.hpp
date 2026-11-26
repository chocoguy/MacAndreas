#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ma::host::input {

constexpr uint8_t kVkShift = 0x10;
constexpr uint8_t kVkControl = 0x11;
constexpr uint8_t kVkMenu = 0x12;
constexpr uint8_t kVkCapital = 0x14;
constexpr uint8_t kVkF10 = 0x79;
constexpr uint8_t kVkLShift = 0xa0;
constexpr uint8_t kVkRShift = 0xa1;
constexpr uint8_t kVkLControl = 0xa2;
constexpr uint8_t kVkRControl = 0xa3;
constexpr uint8_t kVkLMenu = 0xa4;
constexpr uint8_t kVkRMenu = 0xa5;

struct Key {
    std::string_view name;
    uint16_t mac;
    uint8_t vk;
    uint8_t side_vk;
    uint8_t scan;
    bool extended;
};

const Key* key_for_mac(uint16_t mac);
const Key* key_for_name(std::string_view name);
uint8_t dik(const Key& key);
uint8_t other_side(uint8_t side_vk);

enum class StepKind : uint8_t { key, button, move, wheel, focus, look, pad_button, pad_stick };

struct KeyStep {
    bool movie = false;
    bool timed = false;
    uint64_t frame = 0;
    StepKind kind = StepKind::key;
    const Key* key = nullptr;
    unsigned button = 0;
    uint8_t pad_button = 0;
    int32_t wheel = 0;
    bool has_pos = false;
    int32_t x = 0;
    int32_t y = 0;
    bool down = false;
    bool tap = false;
};

std::vector<KeyStep> parse_key_script(std::string_view text, std::string& error);

struct ScriptSink {
    std::function<void(const Key& key, bool down)> key;
    std::function<void(int32_t x, int32_t y)> move;
    std::function<void(unsigned button, bool down)> button;
    std::function<void(int32_t delta)> wheel;
    std::function<void(bool active)> focus;
};

void set_key_script(std::vector<KeyStep> steps);
void set_script_sink(ScriptSink sink);
void run_key_script(bool movie, uint64_t frame);
void poll_key_script();
void note_idle_peek();

}
