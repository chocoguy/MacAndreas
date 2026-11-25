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

std::span<const NativeSymbol> builtin_symbols();
TraceFn find_symbol(std::string_view name);

}
