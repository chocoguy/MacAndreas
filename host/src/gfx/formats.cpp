#include "formats.hpp"

#include "d3d9_defs.hpp"

#include <algorithm>
#include <cstring>

namespace ma::gfx {

namespace {

using namespace d3d;

uint8_t expand(uint32_t v, unsigned bits) {
    if (bits == 0) {
        return 0;
    }
    uint32_t max = (1u << bits) - 1;
    return static_cast<uint8_t>((v * 255 + max / 2) / max);
}

}

FormatInfo format_info(uint32_t f) {
    FormatInfo i;
    switch (f) {
    case fmt::a8r8g8b8:
    case fmt::x8r8g8b8:
        i.texture = true;
        i.render_target = true;
        i.bytes = 4;
        break;
    case fmt::r5g6b5:
    case fmt::x1r5g5b5:
    case fmt::a1r5g5b5:
        i.texture = true;
        i.render_target = true;
        i.bytes = 2;
        break;
    case fmt::a4r4g4b4:
    case fmt::x4r4g4b4:
    case fmt::a8l8:
        i.texture = true;
        i.bytes = 2;
        break;
    case fmt::a8b8g8r8:
    case fmt::x8b8g8r8:
        i.texture = true;
        i.bytes = 4;
        break;
    case fmt::r8g8b8:
        i.texture = true;
        i.bytes = 3;
        break;
    case fmt::a8:
    case fmt::l8:
        i.texture = true;
        i.bytes = 1;
        break;
    case fmt::dxt1:
        i.texture = true;
        i.compressed = true;
        i.bytes = 8;
        i.gpu = GpuFormat::bc1;
        break;
    case fmt::dxt2:
    case fmt::dxt3:
        i.texture = true;
        i.compressed = true;
        i.bytes = 16;
        i.gpu = GpuFormat::bc2;
        break;
    case fmt::dxt4:
    case fmt::dxt5:
        i.texture = true;
        i.compressed = true;
        i.bytes = 16;
        i.gpu = GpuFormat::bc3;
        break;
    case fmt::d16:
    case fmt::d16_lockable:
    case fmt::d24s8:
    case fmt::d24x8:
    case fmt::d24x4s4:
    case fmt::d15s1:
    case fmt::d32:
    case fmt::d24fs8:
    case fmt::d32f_lockable:
        i.depth = true;
        i.bytes = 4;
        i.gpu = GpuFormat::depth;
        break;
    default:
        break;
    }
    return i;
}

uint32_t level_dim(uint32_t base, uint32_t level) { return std::max<uint32_t>(1, base >> level); }

uint32_t level_pitch(uint32_t f, uint32_t width) {
    FormatInfo i = format_info(f);
    if (i.compressed) {
        return std::max<uint32_t>(1, (width + 3) / 4) * i.bytes;
    }
    uint32_t bytes = i.bytes ? i.bytes : 4;
    return (width * bytes + 3) & ~3u;
}

uint32_t level_rows(uint32_t f, uint32_t height) {
    return format_info(f).compressed ? std::max<uint32_t>(1, (height + 3) / 4) : height;
}

uint32_t full_mip_count(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = std::max<uint32_t>(1, width / 2);
        height = std::max<uint32_t>(1, height / 2);
        ++levels;
    }
    return levels;
}

bool needs_conversion(uint32_t f) { return f != fmt::a8r8g8b8 && !format_info(f).compressed; }

void convert_to_bgra8(uint32_t f, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height,
                      uint8_t* dst, uint32_t dst_pitch) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * src_pitch;
        uint8_t* d = dst + static_cast<size_t>(y) * dst_pitch;
        for (uint32_t x = 0; x < width; ++x, d += 4) {
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            uint8_t a = 255;
            switch (f) {
            case fmt::a8r8g8b8:
                b = s[x * 4];
                g = s[x * 4 + 1];
                r = s[x * 4 + 2];
                a = s[x * 4 + 3];
                break;
            case fmt::x8r8g8b8:
                b = s[x * 4];
                g = s[x * 4 + 1];
                r = s[x * 4 + 2];
                break;
            case fmt::a8b8g8r8:
                r = s[x * 4];
                g = s[x * 4 + 1];
                b = s[x * 4 + 2];
                a = s[x * 4 + 3];
                break;
            case fmt::x8b8g8r8:
                r = s[x * 4];
                g = s[x * 4 + 1];
                b = s[x * 4 + 2];
                break;
            case fmt::r8g8b8:
                b = s[x * 3];
                g = s[x * 3 + 1];
                r = s[x * 3 + 2];
                break;
            case fmt::r5g6b5: {
                uint16_t v = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
                r = expand(v >> 11, 5);
                g = expand((v >> 5) & 0x3f, 6);
                b = expand(v & 0x1f, 5);
                break;
            }
            case fmt::x1r5g5b5:
            case fmt::a1r5g5b5: {
                uint16_t v = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
                r = expand((v >> 10) & 0x1f, 5);
                g = expand((v >> 5) & 0x1f, 5);
                b = expand(v & 0x1f, 5);
                a = f == fmt::a1r5g5b5 ? ((v & 0x8000) ? 255 : 0) : 255;
                break;
            }
            case fmt::a4r4g4b4:
            case fmt::x4r4g4b4: {
                uint16_t v = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
                r = expand((v >> 8) & 0xf, 4);
                g = expand((v >> 4) & 0xf, 4);
                b = expand(v & 0xf, 4);
                a = f == fmt::a4r4g4b4 ? expand(v >> 12, 4) : 255;
                break;
            }
            case fmt::a8:
                r = g = b = 0;
                a = s[x];
                break;
            case fmt::l8:
                r = g = b = s[x];
                break;
            case fmt::a8l8:
                r = g = b = s[x * 2];
                a = s[x * 2 + 1];
                break;
            default:
                break;
            }
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = a;
        }
    }
}

}
