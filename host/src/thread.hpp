#pragma once

#include "guest_cpu.hpp"
#include "guest_memory.hpp"

#include <array>
#include <cstdint>

namespace ma::host {

inline constexpr uint32_t kPeb = kSystemArea;
inline constexpr uint32_t kTebBase = kSystemArea + 0x10000;
inline constexpr uint32_t kTebStride = 0x2000;
inline constexpr uint32_t kTlsSlots = 1088;

struct GuestThread {
    GuestState state{};
    uint32_t id = 0;
    uint32_t teb = 0;
    uint32_t stack_bottom = 0;
    uint32_t stack_top = 0;
    uint32_t last_error = 0;
    std::array<uint32_t, kTlsSlots> tls{};
};

void init_process_block(uint32_t image_base, uint32_t process_heap);
GuestThread* create_guest_thread(uint32_t stack_bottom, uint32_t stack_top);
GuestThread* current_thread();
void bind_current_thread(GuestThread* thread);

uint32_t tls_alloc();
bool tls_free(uint32_t slot);

}
