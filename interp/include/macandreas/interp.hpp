#pragma once

#include "guest_cpu.hpp"

#include <cstdint>
#include <string>

namespace ma::interp {

struct Hooks {
    void* context = nullptr;
    bool (*has_native)(void* context, uint32_t pc) = nullptr;
    void (*call_native)(void* context, host::GuestState& s, uint32_t target) = nullptr;
};

enum class Stop {
    returned,
    handed_off,
    unsupported,
};

struct Result {
    Stop stop = Stop::returned;
    uint32_t pc = 0;
    uint64_t steps = 0;
    std::string message;
};

Result run(host::GuestState& s, uint32_t pc, const Hooks& hooks);

std::string describe(uint32_t pc);

}
