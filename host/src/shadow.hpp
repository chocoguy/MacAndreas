#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace ma::host::shadow {

struct Options {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    uint64_t from_frame = 0;
    uint32_t samples = 8;
    uint32_t every = 1;
    bool deep = false;
    std::vector<uint32_t> probes;
    std::vector<std::pair<uint32_t, uint32_t>> touch_ranges;
    uint32_t touch_value = 0;
};

void install(const Options& options);
void note_frame(uint64_t frame);

}
