#pragma once

#include "macandreas/pe_image.hpp"

#include <cstdint>
#include <filesystem>

namespace ma::host {

struct DiffTestOptions {
    std::filesystem::path list;
    uint32_t iterations = 12;
    uint64_t seed = 1;
    bool verbose = false;
    uint32_t x87_args = 0;
    bool deep = false;
};

int run_diff_tests(const pe::Image& image, const DiffTestOptions& options);

}
