#pragma once

#include <cstddef>
#include <cstdint>

#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace ma::recomp {

inline constexpr unsigned kX87ControlWordOffset = 2768;
inline constexpr unsigned kX87StatusWordOffset = 2770;
inline constexpr unsigned kX87StatusFlagsOffset = 2736;

void lower_x86_fp80(llvm::Module& module, std::vector<std::string>& errors);

void fix_x87_control_word_semantics(llvm::Module& semantics, std::vector<std::string>& errors);

void fix_x87_status_word_semantics(llvm::Module& semantics, std::vector<std::string>& errors);

void fix_x87_fpatan_semantics(llvm::Module& semantics, std::vector<std::string>& errors);

void mark_x87_exact_semantics(llvm::Module& semantics, std::vector<std::string>& errors);

size_t preserve_x87_condition_codes(llvm::Module& semantics, std::vector<std::string>& errors);

void rewrite_x86_inline_asm(llvm::Module& module, std::vector<std::string>& errors);

void strip_x86_function_attributes(llvm::Module& module);

void relax_intrinsic_memory_attributes(llvm::Module& module);
void enable_guest_unwinding(llvm::Module& module);
size_t route_calls_through_dispatch(llvm::Module& module);
size_t insert_tso_fences(llvm::Module& module, const std::vector<uint32_t>& functions);

}
