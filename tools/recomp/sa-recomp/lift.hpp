#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace ma::recomp {

struct LiftOptions {
    std::filesystem::path exe;
    std::filesystem::path analysis_dir;
    std::filesystem::path out_dir;
    size_t traces_per_shard = 400;
    size_t limit = 0;
    bool data_pointer_roots = true;
    std::vector<uint32_t> only;
    std::filesystem::path manifest_dir;
};

struct LiftResult {
    std::vector<std::filesystem::path> shards;
    std::filesystem::path functable;
    size_t roots = 0;
    size_t traces = 0;
    size_t stubs = 0;
    std::vector<uint32_t> unresolved_jumps;
    size_t manifest_patches = 0;
    std::vector<uint32_t> tso;
};

std::expected<LiftResult, std::string> lift_program(const LiftOptions& options);

}
