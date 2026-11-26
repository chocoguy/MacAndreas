#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ma::host::perf {

struct FrameStats {
    size_t frames = 0;
    double mean_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
    size_t over_20ms = 0;
    size_t over_33ms = 0;
};

FrameStats summarize(std::span<const float> frame_ms);
std::string format_stats(const FrameStats& stats);
void frame_presented();
void finish();

}
