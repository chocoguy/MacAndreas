#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <vector>
#include <string>

namespace ma::recomp {

inline constexpr const char* kHostTriple = "arm64-apple-macosx26.0.0";
inline constexpr const char* kHostCpu = "apple-m1";

struct CompileOptions {
    std::filesystem::path input;
    std::filesystem::path runtime_bc;
    std::filesystem::path output;
    int opt_level = 2;
    size_t max_optimized_instructions = 200000;
    bool emit_ir = false;
    std::vector<uint32_t> tso;
};

std::expected<void, std::string> compile_module(const CompileOptions& options);

}
