#pragma once

#include "settings.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ma::host::camera {

inline constexpr uint32_t kTheCamera = 0x00b6f028;
inline constexpr uint32_t kSetCamPositionForFixedMode = 0x0050bec0;
inline constexpr uint32_t kTakeControlNoEntity = 0x0050c8b0;
inline constexpr uint32_t kRestore = 0x0050b930;
inline constexpr uint32_t kJumpCut = 2;
inline constexpr uint32_t kScriptControl = 1;

std::optional<Settings::CameraPin> parse_pin(std::string_view text);
void apply_pins(uint64_t frame);

}
