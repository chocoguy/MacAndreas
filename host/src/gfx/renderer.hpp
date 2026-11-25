#pragma once

#include "ff.hpp"
#include "formats.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace ma::gfx {

struct GpuTexture;

struct TextureInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    GpuFormat format = GpuFormat::bgra8;
    bool cube = false;
    bool render_target = false;
    bool opaque = false;
};

struct SamplerDesc {
    uint8_t address_u = 1;
    uint8_t address_v = 1;
    uint8_t address_w = 1;
    uint8_t mag = 1;
    uint8_t min = 1;
    uint8_t mip = 0;
    uint8_t max_anisotropy = 1;
    uint8_t border_white = 0;
    uint8_t max_mip_level = 0;
    uint8_t pad[3] = {};
};

struct StencilFace {
    uint8_t fail = 1;
    uint8_t depth_fail = 1;
    uint8_t pass = 1;
    uint8_t func = 8;
};

struct DepthStencilDesc {
    uint8_t depth_test = 0;
    uint8_t depth_write = 0;
    uint8_t depth_func = 4;
    uint8_t stencil = 0;
    StencilFace front{};
    StencilFace back{};
    uint8_t read_mask = 0xff;
    uint8_t write_mask = 0xff;
    uint8_t pad[2] = {};
};

struct StreamData {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct DrawCall {
    PipelineKey key{};
    VertexUniforms vs{};
    FragmentUniforms fs{};
    std::array<StreamData, kMaxStreams> streams{};
    const uint8_t* indices = nullptr;
    uint32_t index_count = 0;
    bool index32 = false;
    uint32_t vertex_count = 0;
    uint8_t primitive = 0;
    std::array<GpuTexture*, kMaxStages> textures{};
    std::array<SamplerDesc, kMaxStages> samplers{};
    Viewport viewport{};
    bool scissor_enable = false;
    Rect scissor{};
    DepthStencilDesc depth_stencil{};
    uint8_t cull = 1;
    bool wireframe = false;
    float depth_bias = 0;
    float slope_bias = 0;
    uint32_t stencil_ref = 0;
    float blend_color[4] = {};
};

struct RenderTarget {
    GpuTexture* texture = nullptr;
    uint32_t face = 0;
    uint32_t level = 0;
};

struct PresentOptions {
    std::filesystem::path capture_dir;
    std::vector<uint32_t> capture_frames;
    bool capture_all = false;
};

std::string compile_msl(const std::string& source);

class Renderer {
public:
    virtual ~Renderer() = default;

    static std::unique_ptr<Renderer> create_metal(const PresentOptions& options);

    virtual void set_layer(void* metal_layer) = 0;
    virtual GpuTexture* create_texture(const TextureInfo& info) = 0;
    virtual void destroy_texture(GpuTexture* texture) = 0;
    virtual const TextureInfo& texture_info(const GpuTexture* texture) const = 0;
    virtual void upload(GpuTexture* texture, uint32_t face, uint32_t level, const void* data, uint32_t pitch) = 0;
    virtual void set_targets(RenderTarget color, GpuTexture* depth) = 0;
    virtual void clear(uint32_t flags, std::span<const Rect> rects, const Viewport& viewport, uint32_t color, float z,
                       uint32_t stencil) = 0;
    virtual void draw(const DrawCall& call) = 0;
    virtual void stretch(RenderTarget src, const Rect& src_rect, RenderTarget dst, const Rect& dst_rect, bool linear) = 0;
    virtual void generate_mipmaps(GpuTexture* texture) = 0;
    virtual bool read_back(RenderTarget src, void* out, uint32_t pitch) = 0;
    virtual void present(GpuTexture* backbuffer) = 0;
    virtual void present_image(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) = 0;
    virtual uint64_t frame() const = 0;
    virtual uint64_t image_frame() const = 0;
};

}
