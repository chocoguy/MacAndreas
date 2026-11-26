#pragma once

#include "guest_cpu.hpp"

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>

namespace ma::host {

std::expected<size_t, std::string> load_function_table(void* dylib);
TraceFn lookup_trace(uint32_t addr);
void dispatch(GuestState& s, uint32_t target);
void request_jump(GuestState& s, uint32_t target);
void interpret_missing(GuestState& s, uint32_t target);
bool emulate_segment_instruction(GuestState& s);
uint32_t call_guest(GuestState& s, uint32_t target, std::span<const uint32_t> args, bool callee_pops);
uint32_t call_guest(GuestState& s, uint32_t target, std::initializer_list<uint32_t> args, bool callee_pops);
void register_trace(uint32_t addr, TraceFn fn);
void set_trace_interp(bool enabled);
void force_interpret(uint32_t addr);
size_t force_interpret_range(uint32_t low, uint32_t high);
void trace_interpreted_pc(uint32_t addr);
void finish_call(GuestState& s, uint32_t expected_return);
[[noreturn]] void guest_fatal(GuestState& s, const char* kind, uint32_t addr);

}
