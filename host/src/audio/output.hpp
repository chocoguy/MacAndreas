#pragma once

#include <cstdint>
#include <filesystem>

namespace ma::host::audio {

using RenderFn = void (*)(float* interleaved_stereo, uint32_t frames, uint32_t rate);

struct OutputOptions {
    bool silent = false;
    bool clockless = false;
    std::filesystem::path capture;
};

void start_output(RenderFn render, const OutputOptions& options);
void finish_output();
uint32_t output_rate();
OutputOptions default_options();

}
