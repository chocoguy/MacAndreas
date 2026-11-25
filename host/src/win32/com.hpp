#pragma once

#include "../thunks.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace ma::host::win32 {

inline constexpr uint32_t kSOk = 0;
inline constexpr uint32_t kENoInterface = 0x80004002u;
inline constexpr uint32_t kEFail = 0x80004005u;

struct ComClass {
    std::string_view dll;
    std::span<const std::string_view> methods;
};

uint32_t com_create(const ComClass& cls, uint32_t extra_bytes = 0);
uint32_t com_add_ref(uint32_t object);
uint32_t com_release(uint32_t object);
uint32_t com_extra(uint32_t object);

}
