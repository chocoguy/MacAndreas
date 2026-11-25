#pragma once

#include "../guest_cpu.hpp"

#include <cstdint>

namespace ma::host::win32 {

inline constexpr uint32_t kSehChainEnd = 0xffffffffu;
inline constexpr uint32_t kExceptionNoncontinuable = 0x1;
inline constexpr uint32_t kExceptionUnwinding = 0x2;
inline constexpr uint32_t kExceptionExitUnwind = 0x4;
inline constexpr uint32_t kStatusUnwind = 0xc0000027u;
inline constexpr uint32_t kStatusNoncontinuableException = 0xc0000025u;
inline constexpr uint32_t kStatusInvalidDisposition = 0xc0000026u;

inline constexpr uint32_t kExceptionRecordSize = 80;
inline constexpr uint32_t kContextSize = 0x2cc;
inline constexpr uint32_t kContextFull = 0x00010007u;

namespace context_offset {
inline constexpr uint32_t flags = 0x00;
inline constexpr uint32_t edi = 0x9c;
inline constexpr uint32_t esi = 0xa0;
inline constexpr uint32_t ebx = 0xa4;
inline constexpr uint32_t edx = 0xa8;
inline constexpr uint32_t ecx = 0xac;
inline constexpr uint32_t eax = 0xb0;
inline constexpr uint32_t ebp = 0xb4;
inline constexpr uint32_t eip = 0xb8;
inline constexpr uint32_t cs = 0xbc;
inline constexpr uint32_t eflags = 0xc0;
inline constexpr uint32_t esp = 0xc4;
inline constexpr uint32_t ss = 0xc8;
}

enum class Disposition : uint32_t {
    continue_execution = 0,
    continue_search = 1,
    nested_exception = 2,
    collided_unwind = 3,
};

enum class DispatchResult {
    continued,
    unhandled,
};

struct GuestUnwind {
    uint32_t eip;
    uint32_t esp;
};

uint32_t seh_head(GuestState& s);
void set_seh_head(GuestState& s, uint32_t frame);

void write_exception_record(uint32_t record, uint32_t code, uint32_t flags, uint32_t address, uint32_t nested,
                            const uint32_t* params, uint32_t count);
void capture_context(GuestState& s, uint32_t context, uint32_t eip, uint32_t esp);
void restore_context(GuestState& s, uint32_t context);

DispatchResult dispatch_exception(GuestState& s, uint32_t record, uint32_t context);
void unwind_to(GuestState& s, uint32_t target_frame, uint32_t record);

uint32_t unhandled_exception_filter();
uint32_t set_unhandled_exception_filter(uint32_t filter);

bool unwind_matches_call(const GuestUnwind& unwind, uint32_t return_slot, uint32_t return_address);

}
