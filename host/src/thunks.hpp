#pragma once

#include "guest_cpu.hpp"
#include "guest_memory.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ma::host {

struct ApiDef;

struct ApiCall {
    GuestState& s;
    const ApiDef* def = nullptr;

    uint32_t arg(unsigned i) const { return mem::read<uint32_t>(esp(s) + 4 + 4 * i); }
    void ret(uint32_t v) { eax(s) = v; }
    void ret64(uint64_t v) {
        eax(s) = static_cast<uint32_t>(v);
        edx(s) = static_cast<uint32_t>(v >> 32);
    }
};

using ApiFn = void (*)(ApiCall&);

enum class Conv : uint8_t { stdcall, cdecl_, thiscall };

struct ApiDef {
    std::string_view dll;
    std::string_view name;
    uint16_t ordinal;
    uint8_t args;
    Conv conv;
    ApiFn fn;
    uint32_t value = 0;
};

void register_apis(std::span<const ApiDef> defs);
uint32_t thunk_for(std::string_view dll, std::string_view name, uint16_t ordinal);
bool is_thunk(uint32_t addr);
void invoke_thunk(GuestState& s, uint32_t addr);
std::string thunk_name(uint32_t addr);
void set_trace_imports(bool enabled);

}
