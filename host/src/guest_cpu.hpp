#pragma once

#define ADDRESS_SIZE_BITS 32
#define HAS_FEATURE_AVX 0
#define HAS_FEATURE_AVX512 0
#include "remill/Arch/Runtime/HyperCall.h"
#include "remill/Arch/X86/Runtime/State.h"

#include <cstddef>
#include <cstdint>

namespace ma::host {

using GuestState = X86State;
using TraceFn = Memory* (*)(GuestState&, uint32_t, Memory*);

static_assert(sizeof(GuestState) == 3504, "Remill x86 State layout changed; rerun state-layout-check");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(GuestState, x87.fxsave32.cwd) == 2768, "sa-recomp reads the x87 control word at State+2768");
#pragma clang diagnostic pop

inline uint32_t& eax(GuestState& s) { return s.gpr.rax.dword; }
inline uint32_t& ecx(GuestState& s) { return s.gpr.rcx.dword; }
inline uint32_t& edx(GuestState& s) { return s.gpr.rdx.dword; }
inline uint32_t& ebx(GuestState& s) { return s.gpr.rbx.dword; }
inline uint32_t& esp(GuestState& s) { return s.gpr.rsp.dword; }
inline uint32_t& ebp(GuestState& s) { return s.gpr.rbp.dword; }
inline uint32_t& esi(GuestState& s) { return s.gpr.rsi.dword; }
inline uint32_t& edi(GuestState& s) { return s.gpr.rdi.dword; }
inline uint32_t& eip(GuestState& s) { return s.gpr.rip.dword; }

}
