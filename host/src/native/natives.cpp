#include "natives.hpp"

namespace ma::host::native {

namespace {

constexpr uint32_t kDirectX9 = 0x900;

uint32_t get_dx_version() { return kDirectX9; }

constexpr NativeSymbol kSymbols[] = {
    {"ma_get_dx_version", native_trace<Conv::cdecl_, get_dx_version>},
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
