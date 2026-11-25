#pragma once

#include <cstdint>
#include <vector>

namespace ma::gfx {

enum class GpuFormat : uint8_t { bgra8, bc1, bc2, bc3, depth };

struct FormatInfo {
    bool texture = false;
    bool render_target = false;
    bool depth = false;
    bool compressed = false;
    uint32_t bytes = 0;
    GpuFormat gpu = GpuFormat::bgra8;
};

FormatInfo format_info(uint32_t d3d_format);
uint32_t level_dim(uint32_t base, uint32_t level);
uint32_t level_pitch(uint32_t d3d_format, uint32_t width);
uint32_t level_rows(uint32_t d3d_format, uint32_t height);
uint32_t full_mip_count(uint32_t width, uint32_t height);

bool needs_conversion(uint32_t d3d_format);
void convert_to_bgra8(uint32_t d3d_format, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height,
                      uint8_t* dst, uint32_t dst_pitch);

}
