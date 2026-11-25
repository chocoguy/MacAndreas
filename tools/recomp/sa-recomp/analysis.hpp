#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ma::recomp {

struct FunctionEntry {
    uint32_t entry = 0;
    uint32_t end = 0;
    std::string section;
};

struct Analysis {
    std::string exe_sha256;
    std::vector<FunctionEntry> functions;
    std::map<uint32_t, std::vector<uint32_t>> jump_targets;
    std::vector<uint32_t> unresolved_jumps;
    std::vector<uint32_t> extra_roots;
};

std::expected<Analysis, std::string> load_analysis(const std::filesystem::path& dir);

}
