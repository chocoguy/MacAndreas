#include "natives.hpp"

#include "../thread.hpp"

#include <cstdio>
#include <format>
#include <mutex>
#include <vector>

namespace ma::host::native {

namespace {

constexpr uint32_t kDirectX9 = 0x900;
constexpr uint32_t kRwEngineGetNumVideoModes = 0x007f2cc0;
constexpr uint32_t kRwEngineGetVideoModeInfo = 0x007f2cf0;
constexpr uint32_t kVideoModeListGlobal = 0x00c920d0;
constexpr uint32_t kRwVideoModeExclusive = 0x1;
constexpr uint32_t kLabelSize = 100;

uint32_t get_dx_version() { return kDirectX9; }

uint32_t get_video_mode_list() {
    static std::mutex mutex;
    static std::vector<uint32_t> labels;
    static uint32_t list = 0;
    std::lock_guard lock(mutex);
    GuestState& s = current_thread()->state;
    auto count = guest_call<Conv::cdecl_, int32_t>(s, kRwEngineGetNumVideoModes);
    if (count <= 0) {
        return 0;
    }
    if (list != 0 && labels.size() == static_cast<size_t>(count)) {
        mem::write<uint32_t>(kVideoModeListGlobal, list);
        return list;
    }
    for (uint32_t label : labels) {
        allocator().free(label);
    }
    labels.assign(static_cast<size_t>(count), 0);
    allocator().free(list);
    list = allocator().alloc(static_cast<uint32_t>(count) * 4);
    uint32_t info = allocator().alloc(24);
    for (int32_t i = 0; i < count; ++i) {
        guest_call<Conv::cdecl_, uint32_t>(s, kRwEngineGetVideoModeInfo, info, i);
        uint32_t width = mem::read<uint32_t>(info);
        uint32_t height = mem::read<uint32_t>(info + 4);
        uint32_t depth = mem::read<uint32_t>(info + 8);
        uint32_t flags = mem::read<uint32_t>(info + 12);
        if ((flags & kRwVideoModeExclusive) == 0 || width < 640 || height < 480) {
            continue;
        }
        labels[static_cast<size_t>(i)] = allocator().alloc(kLabelSize);
        mem::write_cstr(labels[static_cast<size_t>(i)], std::format("{} X {} X {}", width, height, depth));
        mem::write<uint32_t>(list + 4u * static_cast<uint32_t>(i), labels[static_cast<size_t>(i)]);
    }
    allocator().free(info);
    mem::write<uint32_t>(kVideoModeListGlobal, list);
    return list;
}

constexpr NativeSymbol kSymbols[] = {
    {"ma_get_dx_version", native_trace<Conv::cdecl_, get_dx_version>},
    {"ma_get_video_mode_list", native_trace<Conv::cdecl_, get_video_mode_list>},
};

}

std::span<const NativeSymbol> builtin_symbols() { return kSymbols; }

TraceFn find_symbol(std::string_view name) {
    for (const auto& symbol : kSymbols) {
        if (symbol.name == name) {
            return symbol.fn;
        }
    }
    return nullptr;
}

}
