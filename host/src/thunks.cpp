#include "thunks.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ma::host {

namespace {

struct Thunk {
    std::string dll;
    std::string name;
    const ApiDef* def = nullptr;
};

std::mutex g_mutex;
std::vector<const ApiDef*> g_defs;
std::unordered_map<std::string, const ApiDef*> g_by_key;
std::deque<Thunk> g_thunks;
std::unordered_map<std::string, uint32_t> g_thunk_by_key;
bool g_trace = false;

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string key(std::string_view dll, std::string_view name, uint16_t ordinal) {
    std::string d = lower(dll);
    if (!d.ends_with(".dll")) {
        d += ".dll";
    }
    return name.empty() ? std::format("{}!#{}", d, ordinal) : std::format("{}!{}", d, name);
}

}

void register_apis(std::span<const ApiDef> defs) {
    std::lock_guard lock(g_mutex);
    for (const auto& def : defs) {
        g_by_key[key(def.dll, def.name, def.ordinal)] = &def;
    }
}

uint32_t thunk_for(std::string_view dll, std::string_view name, uint16_t ordinal) {
    std::lock_guard lock(g_mutex);
    auto k = key(dll, name, ordinal);
    if (auto it = g_thunk_by_key.find(k); it != g_thunk_by_key.end()) {
        return it->second;
    }
    Thunk t;
    t.dll = std::string(dll);
    t.name = name.empty() ? std::format("#{}", ordinal) : std::string(name);
    if (auto it = g_by_key.find(k); it != g_by_key.end()) {
        t.def = it->second;
    }
    uint32_t addr = kThunkBase + static_cast<uint32_t>(g_thunks.size()) * 4;
    g_thunks.push_back(std::move(t));
    g_thunk_by_key[k] = addr;
    return addr;
}

bool is_thunk(uint32_t addr) { return addr >= kThunkBase && addr < kThunkEnd; }

std::string thunk_name(uint32_t addr) {
    std::lock_guard lock(g_mutex);
    uint32_t index = (addr - kThunkBase) / 4;
    if (!is_thunk(addr) || index >= g_thunks.size()) {
        return std::format("<bad thunk 0x{:08x}>", addr);
    }
    return g_thunks[index].dll + "!" + g_thunks[index].name;
}

void set_trace_imports(bool enabled) { g_trace = enabled; }

void invoke_thunk(GuestState& s, uint32_t addr) {
    const Thunk* thunk = nullptr;
    {
        std::lock_guard lock(g_mutex);
        uint32_t index = (addr - kThunkBase) / 4;
        if (index < g_thunks.size()) {
            thunk = &g_thunks[index];
        }
    }
    uint32_t return_address = mem::read<uint32_t>(esp(s));
    if (!thunk || !thunk->def) {
        std::fprintf(stderr, "[IMPORT] %s not implemented (called from 0x%08x)\n", thunk_name(addr).c_str(),
                     return_address);
        std::fflush(stderr);
        std::_Exit(3);
    }
    if (g_trace) {
        std::fprintf(stderr, "[API] %s!%s from 0x%08x\n", thunk->dll.c_str(), thunk->name.c_str(), return_address);
    }
    ApiCall call{s, thunk->def};
    thunk->def->fn(call);
    esp(s) += 4 + (thunk->def->conv != Conv::cdecl_ ? 4u * thunk->def->args : 0u);
    eip(s) = return_address;
}

}
