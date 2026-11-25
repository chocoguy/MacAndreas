#pragma once

#include <cstddef>

#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace ma::recomp {

void lower_x86_fp80(llvm::Module& module, std::vector<std::string>& errors);

void rewrite_x86_inline_asm(llvm::Module& module, std::vector<std::string>& errors);

void strip_x86_function_attributes(llvm::Module& module);

void relax_intrinsic_memory_attributes(llvm::Module& module);
void enable_guest_unwinding(llvm::Module& module);
size_t route_calls_through_dispatch(llvm::Module& module);

}
