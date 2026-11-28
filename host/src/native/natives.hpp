#pragma once

#include "../abi.hpp"

#include <span>
#include <string_view>

namespace ma::host::native {

struct NativeSymbol {
    std::string_view name;
    TraceFn fn;
};

template <Conv C, auto Fn>
Memory* native_trace(GuestState& s, uint32_t, Memory* m) {
    ApiCall call{s};
    NativeExport<C, Fn>::invoke(call);
    uint32_t return_address = mem::read<uint32_t>(esp(s));
    esp(s) += 4 + (C == Conv::cdecl_ ? 0u : 4u * NativeExport<C, Fn>::stack_slots());
    eip(s) = return_address;
    return m;
}

struct MidHookSymbol {
    std::string_view name;
    MidHookFn fn;
};

using SlotFn = float (*)(float original);

struct SlotSymbol {
    std::string_view name;
    SlotFn fn;
};

std::span<const NativeSymbol> builtin_symbols();
TraceFn find_symbol(std::string_view name);
std::span<const NativeSymbol> silentpatch_symbols();
std::span<const MidHookSymbol> silentpatch_mid_hooks();
std::span<const NativeSymbol> widescreen_symbols();
std::span<const MidHookSymbol> widescreen_mid_hooks();
std::span<const NativeSymbol> widescreen_hud_symbols();
std::span<const MidHookSymbol> widescreen_hud_mid_hooks();
MidHookFn find_mid_hook(std::string_view name);
std::span<const SlotSymbol> silentpatch_slots();
std::span<const SlotSymbol> widescreen_slots();
std::span<const SlotSymbol> widescreen_hud_slots();
SlotFn find_slot(std::string_view name);

}
