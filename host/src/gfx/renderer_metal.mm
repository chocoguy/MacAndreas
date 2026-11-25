#include "renderer.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ma::gfx {

struct GpuTexture {
    TextureInfo info;
    id<MTLTexture> texture = nil;
    id<MTLTexture> sample_view = nil;
    uint64_t used_serial = 0;
};

namespace {

constexpr NSUInteger kChunkSize = 8u << 20;
constexpr NSUInteger kVertexUniformIndex = 8;
constexpr int kFramesInFlight = 3;

const char* kUtilitySource = R"(
#include <metal_stdlib>
using namespace metal;
struct QuadOut { float4 pos [[position]]; float2 uv; };
struct QuadU { float4 dst; float4 src; };
vertex QuadOut quad_vs(uint vid [[vertex_id]], constant QuadU& u [[buffer(0)]]) {
  float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
  QuadOut o;
  o.pos = float4(mix(u.dst.xy, u.dst.zw, c), 0.0, 1.0);
  o.uv = mix(u.src.xy, u.src.zw, c);
  return o;
}
fragment float4 quad_fs(QuadOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, in.uv);
}
struct ClearOut { float4 pos [[position]]; };
struct ClearU { float4 color; float4 z; };
vertex ClearOut clear_vs(uint vid [[vertex_id]], constant ClearU& u [[buffer(0)]]) {
  ClearOut o;
  o.pos = float4(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0, u.z.x, 1.0);
  return o;
}
fragment float4 clear_fs(constant ClearU& u [[buffer(0)]]) { return u.color; }
)";

struct QuadUniforms {
    float dst[4];
    float src[4];
};

struct ClearUniforms {
    float color[4];
    float z[4];
};

MTLPixelFormat pixel_format(GpuFormat f) {
    switch (f) {
    case GpuFormat::bc1: return MTLPixelFormatBC1_RGBA;
    case GpuFormat::bc2: return MTLPixelFormatBC2_RGBA;
    case GpuFormat::bc3: return MTLPixelFormatBC3_RGBA;
    case GpuFormat::depth: return MTLPixelFormatDepth32Float_Stencil8;
    default: return MTLPixelFormatBGRA8Unorm;
    }
}

MTLVertexFormat vertex_format(uint8_t type) {
    using namespace d3d::decltype_;
    switch (type) {
    case float1: return MTLVertexFormatFloat;
    case float2: return MTLVertexFormatFloat2;
    case float3: return MTLVertexFormatFloat3;
    case float4: return MTLVertexFormatFloat4;
    case d3dcolor: return MTLVertexFormatUChar4Normalized_BGRA;
    case ubyte4: return MTLVertexFormatUChar4;
    case short2: return MTLVertexFormatShort2;
    case short4: return MTLVertexFormatShort4;
    case ubyte4n: return MTLVertexFormatUChar4Normalized;
    case short2n: return MTLVertexFormatShort2Normalized;
    case short4n: return MTLVertexFormatShort4Normalized;
    case ushort2n: return MTLVertexFormatUShort2Normalized;
    case ushort4n: return MTLVertexFormatUShort4Normalized;
    case udec3: return MTLVertexFormatUInt1010102Normalized;
    case dec3n: return MTLVertexFormatInt1010102Normalized;
    case float16_2: return MTLVertexFormatHalf2;
    case float16_4: return MTLVertexFormatHalf4;
    default: return MTLVertexFormatInvalid;
    }
}

MTLBlendFactor blend_factor(uint8_t f) {
    using namespace d3d::blend;
    switch (f) {
    case zero: return MTLBlendFactorZero;
    case one: return MTLBlendFactorOne;
    case srccolor: return MTLBlendFactorSourceColor;
    case invsrccolor: return MTLBlendFactorOneMinusSourceColor;
    case srcalpha: return MTLBlendFactorSourceAlpha;
    case invsrcalpha: return MTLBlendFactorOneMinusSourceAlpha;
    case destalpha: return MTLBlendFactorDestinationAlpha;
    case invdestalpha: return MTLBlendFactorOneMinusDestinationAlpha;
    case destcolor: return MTLBlendFactorDestinationColor;
    case invdestcolor: return MTLBlendFactorOneMinusDestinationColor;
    case srcalphasat: return MTLBlendFactorSourceAlphaSaturated;
    case blendfactor: return MTLBlendFactorBlendColor;
    case invblendfactor: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blend_operation(uint8_t op) {
    switch (op) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction compare(uint8_t f) {
    if (f < 1 || f > 8) {
        return MTLCompareFunctionAlways;
    }
    return static_cast<MTLCompareFunction>(f - 1);
}

MTLStencilOperation stencil_op(uint8_t op) {
    switch (op) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode address_mode(uint8_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

MTLColorWriteMask write_mask(uint8_t d3d) {
    MTLColorWriteMask m = MTLColorWriteMaskNone;
    if (d3d & 1) m |= MTLColorWriteMaskRed;
    if (d3d & 2) m |= MTLColorWriteMaskGreen;
    if (d3d & 4) m |= MTLColorWriteMaskBlue;
    if (d3d & 8) m |= MTLColorWriteMaskAlpha;
    return m;
}

struct Staging {
    id<MTLBuffer> buffer;
    NSUInteger offset;
    uint8_t* cpu;
};

bool write_png(const std::filesystem::path& path, const uint8_t* bgra, uint32_t width, uint32_t height,
               uint32_t pitch) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, bgra, static_cast<CFIndex>(pitch) * height);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(width, height, 8, 32, pitch, space,
                                     static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dest && image) {
        CGImageDestinationAddImage(dest, image, nullptr);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    if (image) CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return ok;
}

class MetalRenderer final : public Renderer {
public:
    explicit MetalRenderer(const PresentOptions& options) : options_(options) {
        device_ = MTLCreateSystemDefaultDevice();
        queue_ = [device_ newCommandQueue];
        frame_sem_ = dispatch_semaphore_create(kFramesInFlight);
        NSError* error = nil;
        utility_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kUtilitySource] options:nil error:&error];
        if (!utility_) {
            std::fprintf(stderr, "[METAL] utility shaders failed: %s\n", error.localizedDescription.UTF8String);
        }
        if (!options_.capture_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(options_.capture_dir, ec);
        }
        std::fprintf(stderr, "[METAL] device %s\n", device_.name.UTF8String);
    }

    ~MetalRenderer() override {
        end_encoder();
        submit();
        if (last_cmd_) {
            [last_cmd_ waitUntilCompleted];
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (pending_captures_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void set_layer(void* layer) override {
        layer_ = (__bridge CAMetalLayer*)layer;
        if (layer_) {
            layer_.device = device_;
            layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer_.framebufferOnly = YES;
        }
    }

    GpuTexture* create_texture(const TextureInfo& info) override {
        auto* t = new GpuTexture;
        t->info = info;
        MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
        d.textureType = info.cube ? MTLTextureTypeCube : MTLTextureType2D;
        d.pixelFormat = pixel_format(info.format);
        d.width = std::max<uint32_t>(info.width, 1);
        d.height = std::max<uint32_t>(info.height, 1);
        d.mipmapLevelCount = std::max<uint32_t>(info.levels, 1);
        d.usage = MTLTextureUsageShaderRead;
        if (info.render_target || info.format == GpuFormat::depth) {
            d.usage |= MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
        } else {
            d.storageMode = MTLStorageModeShared;
        }
        if (info.opaque && info.format == GpuFormat::bgra8) {
            d.usage |= MTLTextureUsagePixelFormatView;
        }
        t->texture = [device_ newTextureWithDescriptor:d];
        if (!t->texture) {
            std::fprintf(stderr, "[METAL] texture %ux%u levels=%u failed\n", info.width, info.height, info.levels);
        }
        if (t->texture && info.opaque && info.format == GpuFormat::bgra8) {
            MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
            t->sample_view = [t->texture newTextureViewWithPixelFormat:d.pixelFormat
                                                           textureType:d.textureType
                                                                levels:NSMakeRange(0, d.mipmapLevelCount)
                                                                slices:NSMakeRange(0, info.cube ? 6 : 1)
                                                               swizzle:sw];
        }
        return t;
    }

    void destroy_texture(GpuTexture* t) override {
        if (!t) {
            return;
        }
        if (t->used_serial != 0 && cmd_ && t->used_serial == serial_) {
            retired_.push_back(t->texture);
        }
        delete t;
    }

    const TextureInfo& texture_info(const GpuTexture* t) const override { return t->info; }

    void upload(GpuTexture* t, uint32_t face, uint32_t level, const void* data, uint32_t pitch) override {
        if (!t || !t->texture || level >= t->texture.mipmapLevelCount) {
            return;
        }
        uint32_t w = level_dim(t->info.width, level);
        uint32_t h = level_dim(t->info.height, level);
        bool compressed = t->info.format != GpuFormat::bgra8;
        uint32_t rows = compressed ? std::max<uint32_t>(1, (h + 3) / 4) : h;
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        bool busy = t->used_serial != 0 && t->used_serial > completed_.load();
        if (!busy && t->texture.storageMode == MTLStorageModeShared) {
            [t->texture replaceRegion:region mipmapLevel:level slice:face withBytes:data bytesPerRow:pitch bytesPerImage:0];
            return;
        }
        size_t size = static_cast<size_t>(pitch) * rows;
        Staging st = stage(size, 256);
        std::memcpy(st.cpu, data, size);
        end_encoder();
        ensure_cmd();
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromBuffer:st.buffer
                   sourceOffset:st.offset
              sourceBytesPerRow:pitch
            sourceBytesPerImage:size
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:t->texture
               destinationSlice:face
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        t->used_serial = serial_;
    }

    void set_targets(RenderTarget color, GpuTexture* depth) override {
        if (color.texture == color_.texture && color.face == color_.face && color.level == color_.level &&
            depth == depth_) {
            return;
        }
        end_encoder();
        color_ = color;
        depth_ = depth;
    }

    void clear(uint32_t flags, std::span<const Rect> rects, const Viewport& vp, uint32_t color, float z,
               uint32_t stencil) override {
        bool want_color = (flags & d3d::clear::target) && color_.texture;
        bool want_depth = (flags & d3d::clear::zbuffer) && depth_;
        bool want_stencil = (flags & d3d::clear::stencil) && depth_;
        if (!want_color && !want_depth && !want_stencil) {
            return;
        }
        if (!ensure_encoder()) {
            return;
        }
        id<MTLRenderPipelineState> ps = clear_pipeline(want_color);
        if (!ps) {
            return;
        }
        [enc_ setRenderPipelineState:ps];
        current_pipeline_ = nil;
        [enc_ setDepthStencilState:clear_depth_state(want_depth, want_stencil)];
        [enc_ setStencilReferenceValue:stencil & 0xff];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
        [enc_ setDepthBias:0 slopeScale:0 clamp:0];
        auto [tw, th] = target_size();
        MTLViewport full{0, 0, static_cast<double>(tw), static_cast<double>(th), 0, 1};
        [enc_ setViewport:full];
        ClearUniforms u{};
        color_to_float4(color, u.color);
        u.z[0] = z;
        [enc_ setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc_ setFragmentBytes:&u length:sizeof(u) atIndex:0];
        Rect area{static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y), static_cast<int32_t>(vp.x + vp.width),
                  static_cast<int32_t>(vp.y + vp.height)};
        auto draw_rect = [&](Rect r) {
            r.left = std::max({r.left, area.left, 0});
            r.top = std::max({r.top, area.top, 0});
            r.right = std::min({r.right, area.right, static_cast<int32_t>(tw)});
            r.bottom = std::min({r.bottom, area.bottom, static_cast<int32_t>(th)});
            if (r.right <= r.left || r.bottom <= r.top) {
                return;
            }
            MTLScissorRect sc{static_cast<NSUInteger>(r.left), static_cast<NSUInteger>(r.top),
                              static_cast<NSUInteger>(r.right - r.left), static_cast<NSUInteger>(r.bottom - r.top)};
            [enc_ setScissorRect:sc];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        };
        if (rects.empty()) {
            draw_rect(area);
        } else {
            for (const auto& r : rects) {
                draw_rect(r);
            }
        }
        [enc_ setScissorRect:MTLScissorRect{0, 0, tw, th}];
    }

    void draw(const DrawCall& c) override {
        if (!ensure_encoder()) {
            return;
        }
        id<MTLRenderPipelineState> ps = pipeline(c.key);
        if (!ps) {
            return;
        }
        if (ps != current_pipeline_) {
            [enc_ setRenderPipelineState:ps];
            current_pipeline_ = ps;
        }
        std::array<bool, kMaxStreams> used{};
        for (unsigned i = 0; i < c.key.shader.element_count; ++i) {
            used[c.key.shader.elements[i].stream] = true;
        }
        for (unsigned i = 0; i < kMaxStreams; ++i) {
            if (!used[i]) {
                continue;
            }
            const StreamData& sd = c.streams[i];
            if (!sd.data || sd.size == 0) {
                static uint32_t warned = 0;
                if (warned++ < 8) {
                    std::fprintf(stderr, "[METAL] draw skipped: stream %u has no data\n", i);
                }
                return;
            }
            Staging st = stage(sd.size, 16);
            std::memcpy(st.cpu, sd.data, sd.size);
            [enc_ setVertexBuffer:st.buffer offset:st.offset atIndex:i];
        }
        [enc_ setVertexBytes:&c.vs length:sizeof(c.vs) atIndex:kVertexUniformIndex];
        [enc_ setFragmentBytes:&c.fs length:sizeof(c.fs) atIndex:0];
        for (unsigned i = 0; i < c.key.shader.stage_count; ++i) {
            if (!c.key.shader.stages[i].texture) {
                continue;
            }
            GpuTexture* t = c.textures[i];
            if (!t || !t->texture) {
                return;
            }
            [enc_ setFragmentTexture:(t->sample_view ? t->sample_view : t->texture) atIndex:i];
            [enc_ setFragmentSamplerState:sampler(c.samplers[i], t->texture.mipmapLevelCount > 1) atIndex:i];
            t->used_serial = serial_;
        }
        [enc_ setDepthStencilState:depth_state(c.depth_stencil)];
        [enc_ setStencilReferenceValue:c.stencil_ref & 0xff];
        [enc_ setBlendColorRed:c.blend_color[0] green:c.blend_color[1] blue:c.blend_color[2] alpha:c.blend_color[3]];
        auto [tw, th] = target_size();
        MTLViewport vp{static_cast<double>(c.viewport.x), static_cast<double>(c.viewport.y),
                       static_cast<double>(std::max<uint32_t>(c.viewport.width, 1)),
                       static_cast<double>(std::max<uint32_t>(c.viewport.height, 1)), c.viewport.min_z,
                       c.viewport.max_z};
        [enc_ setViewport:vp];
        Rect sc{0, 0, static_cast<int32_t>(tw), static_cast<int32_t>(th)};
        if (c.scissor_enable) {
            sc.left = std::clamp(c.scissor.left, 0, static_cast<int32_t>(tw));
            sc.top = std::clamp(c.scissor.top, 0, static_cast<int32_t>(th));
            sc.right = std::clamp(c.scissor.right, sc.left, static_cast<int32_t>(tw));
            sc.bottom = std::clamp(c.scissor.bottom, sc.top, static_cast<int32_t>(th));
            if (sc.right == sc.left || sc.bottom == sc.top) {
                return;
            }
        }
        [enc_ setScissorRect:MTLScissorRect{static_cast<NSUInteger>(sc.left), static_cast<NSUInteger>(sc.top),
                                             static_cast<NSUInteger>(sc.right - sc.left),
                                             static_cast<NSUInteger>(sc.bottom - sc.top)}];
        [enc_ setFrontFacingWinding:MTLWindingClockwise];
        [enc_ setCullMode:c.cull == d3d::cull::cw    ? MTLCullModeFront
                          : c.cull == d3d::cull::ccw ? MTLCullModeBack
                                                     : MTLCullModeNone];
        [enc_ setTriangleFillMode:c.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [enc_ setDepthBias:c.depth_bias slopeScale:c.slope_bias clamp:0];

        MTLPrimitiveType type = MTLPrimitiveTypeTriangle;
        switch (c.primitive) {
        case d3d::prim::pointlist: type = MTLPrimitiveTypePoint; break;
        case d3d::prim::linelist: type = MTLPrimitiveTypeLine; break;
        case d3d::prim::linestrip: type = MTLPrimitiveTypeLineStrip; break;
        case d3d::prim::trianglestrip: type = MTLPrimitiveTypeTriangleStrip; break;
        default: type = MTLPrimitiveTypeTriangle; break;
        }
        if (c.primitive == d3d::prim::trianglefan) {
            uint32_t n = c.indices ? c.index_count : c.vertex_count;
            if (n < 3) {
                return;
            }
            uint32_t tris = n - 2;
            Staging st = stage(static_cast<size_t>(tris) * 3 * 4, 4);
            auto* out = reinterpret_cast<uint32_t*>(st.cpu);
            auto index_at = [&](uint32_t i) -> uint32_t {
                if (!c.indices) {
                    return i;
                }
                if (c.index32) {
                    uint32_t v;
                    std::memcpy(&v, c.indices + 4 * i, 4);
                    return v;
                }
                uint16_t v;
                std::memcpy(&v, c.indices + 2 * i, 2);
                return v;
            };
            for (uint32_t t = 0; t < tris; ++t) {
                out[3 * t] = index_at(0);
                out[3 * t + 1] = index_at(t + 1);
                out[3 * t + 2] = index_at(t + 2);
            }
            [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:tris * 3
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:st.buffer
                      indexBufferOffset:st.offset];
            return;
        }
        if (c.indices) {
            size_t bytes = static_cast<size_t>(c.index_count) * (c.index32 ? 4 : 2);
            Staging st = stage(bytes, 4);
            std::memcpy(st.cpu, c.indices, bytes);
            [enc_ drawIndexedPrimitives:type
                             indexCount:c.index_count
                              indexType:c.index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:st.buffer
                      indexBufferOffset:st.offset];
        } else {
            [enc_ drawPrimitives:type vertexStart:0 vertexCount:c.vertex_count];
        }
    }

    void stretch(RenderTarget src, const Rect& sr, RenderTarget dst, const Rect& dr, bool linear) override {
        if (!src.texture || !dst.texture || !src.texture->texture || !dst.texture->texture ||
            src.texture->info.format != GpuFormat::bgra8 || dst.texture->info.format != GpuFormat::bgra8) {
            return;
        }
        end_encoder();
        ensure_cmd();
        uint32_t sw = level_dim(src.texture->info.width, src.level);
        uint32_t sh = level_dim(src.texture->info.height, src.level);
        uint32_t dw = level_dim(dst.texture->info.width, dst.level);
        uint32_t dh = level_dim(dst.texture->info.height, dst.level);
        id<MTLTexture> view = [src.texture->texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                     textureType:MTLTextureType2D
                                                                          levels:NSMakeRange(src.level, 1)
                                                                          slices:NSMakeRange(src.face, 1)];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst.texture->texture;
        rp.colorAttachments[0].level = dst.level;
        rp.colorAttachments[0].slice = dst.face;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:quad_pipeline()];
        [e setViewport:MTLViewport{0, 0, static_cast<double>(dw), static_cast<double>(dh), 0, 1}];
        QuadUniforms u{};
        u.dst[0] = static_cast<float>(dr.left) / dw * 2 - 1;
        u.dst[1] = 1 - static_cast<float>(dr.top) / dh * 2;
        u.dst[2] = static_cast<float>(dr.right) / dw * 2 - 1;
        u.dst[3] = 1 - static_cast<float>(dr.bottom) / dh * 2;
        u.src[0] = static_cast<float>(sr.left) / sw;
        u.src[1] = static_cast<float>(sr.top) / sh;
        u.src[2] = static_cast<float>(sr.right) / sw;
        u.src[3] = static_cast<float>(sr.bottom) / sh;
        [e setVertexBytes:&u length:sizeof(u) atIndex:0];
        [e setFragmentTexture:view atIndex:0];
        SamplerDesc sd{};
        sd.address_u = sd.address_v = 3;
        sd.mag = sd.min = linear ? 2 : 1;
        [e setFragmentSamplerState:sampler(sd, false) atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        src.texture->used_serial = serial_;
        dst.texture->used_serial = serial_;
    }

    void generate_mipmaps(GpuTexture* t) override {
        if (!t || !t->texture || t->texture.mipmapLevelCount < 2 || t->info.format != GpuFormat::bgra8) {
            return;
        }
        end_encoder();
        ensure_cmd();
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit generateMipmapsForTexture:t->texture];
        [blit endEncoding];
        t->used_serial = serial_;
    }

    bool read_back(RenderTarget src, void* out, uint32_t pitch) override {
        if (!src.texture || !src.texture->texture || src.texture->info.format != GpuFormat::bgra8) {
            return false;
        }
        uint32_t w = level_dim(src.texture->info.width, src.level);
        uint32_t h = level_dim(src.texture->info.height, src.level);
        end_encoder();
        ensure_cmd();
        id<MTLBuffer> buf = [device_ newBufferWithLength:static_cast<NSUInteger>(w) * 4 * h
                                                 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:src.texture->texture
                         sourceSlice:src.face
                         sourceLevel:src.level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:static_cast<NSUInteger>(w) * 4 * h];
        [blit endEncoding];
        id<MTLCommandBuffer> done = cmd_;
        submit();
        [done waitUntilCompleted];
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(static_cast<uint8_t*>(out) + static_cast<size_t>(y) * pitch,
                        static_cast<uint8_t*>(buf.contents) + static_cast<size_t>(y) * w * 4, w * 4);
        }
        return true;
    }

    void present(GpuTexture* bb) override {
        end_encoder();
        ensure_cmd();
        ++frame_;
        show(bb);
        if (should_capture(frame_)) {
            capture(bb, options_.capture_dir / std::format("frame_{:05}.png", frame_));
        }
        submit();
    }

    void present_image(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) override {
        if (!image_ || image_->info.width != width || image_->info.height != height) {
            destroy_texture(image_);
            TextureInfo info;
            info.width = width;
            info.height = height;
            info.opaque = true;
            image_ = create_texture(info);
        }
        upload(image_, 0, 0, bgra, pitch);
        end_encoder();
        ensure_cmd();
        ++image_frame_;
        show(image_);
        if (should_capture(image_frame_)) {
            capture(image_, options_.capture_dir / std::format("movie_{:05}.png", image_frame_));
        }
        submit();
    }

    uint64_t frame() const override { return frame_; }
    uint64_t image_frame() const override { return image_frame_; }

private:
    bool should_capture(uint64_t n) const {
        return !options_.capture_dir.empty() &&
               (options_.capture_all || std::ranges::find(options_.capture_frames, n) != options_.capture_frames.end());
    }

    void show(GpuTexture* t) {
        if (!t || !t->texture || !layer_) {
            return;
        }
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (drawable) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
            double dw = static_cast<double>(drawable.texture.width);
            double dh = static_cast<double>(drawable.texture.height);
            double scale = std::min(dw / t->info.width, dh / t->info.height);
            double vw = t->info.width * scale;
            double vh = t->info.height * scale;
            [e setViewport:MTLViewport{(dw - vw) / 2, (dh - vh) / 2, vw, vh, 0, 1}];
            [e setRenderPipelineState:quad_pipeline()];
            QuadUniforms u{{-1, 1, 1, -1}, {0, 0, 1, 1}};
            [e setVertexBytes:&u length:sizeof(u) atIndex:0];
            [e setFragmentTexture:t->texture atIndex:0];
            SamplerDesc sd{};
            sd.address_u = sd.address_v = 3;
            sd.mag = sd.min = 2;
            [e setFragmentSamplerState:sampler(sd, false) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
            [cmd_ presentDrawable:drawable];
        }
        t->used_serial = serial_;
    }

    void capture(GpuTexture* t, std::filesystem::path path) {
        if (!t || !t->texture) {
            return;
        }
        uint32_t w = t->info.width;
        uint32_t h = t->info.height;
        id<MTLBuffer> buf = [device_ newBufferWithLength:static_cast<NSUInteger>(w) * 4 * h
                                                 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:t->texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:static_cast<NSUInteger>(w) * 4 * h];
        [blit endEncoding];
        ++pending_captures_;
        std::atomic<int>* pending = &pending_captures_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) {
          std::vector<uint8_t> pixels(static_cast<const uint8_t*>(buf.contents),
                                      static_cast<const uint8_t*>(buf.contents) + static_cast<size_t>(w) * 4 * h);
          for (size_t i = 3; i < pixels.size(); i += 4) {
              pixels[i] = 255;
          }
          if (write_png(path, pixels.data(), w, h, w * 4)) {
              std::fprintf(stderr, "[CAPTURE] %s\n", path.c_str());
          }
          --*pending;
        }];
    }

    void ensure_cmd() {
        if (cmd_) {
            return;
        }
        dispatch_semaphore_wait(frame_sem_, DISPATCH_TIME_FOREVER);
        cmd_ = [queue_ commandBuffer];
        serial_ = ++serial_counter_;
        uint64_t serial = serial_;
        dispatch_semaphore_t sem = frame_sem_;
        auto* self = this;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> done) {
          if (done.status == MTLCommandBufferStatusError) {
              std::fprintf(stderr, "[METAL] command buffer failed: %s\n", done.error.localizedDescription.UTF8String);
          }
          self->retire(serial);
          dispatch_semaphore_signal(sem);
        }];
    }

    void submit() {
        if (!cmd_) {
            return;
        }
        {
            std::lock_guard lock(pool_mutex_);
            in_flight_[serial_] = std::move(frame_chunks_);
            frame_chunks_.clear();
            in_flight_retired_[serial_] = std::move(retired_);
            retired_.clear();
        }
        current_chunk_ = nil;
        chunk_used_ = 0;
        [cmd_ commit];
        last_cmd_ = cmd_;
        cmd_ = nil;
    }

    void retire(uint64_t serial) {
        std::lock_guard lock(pool_mutex_);
        if (auto it = in_flight_.find(serial); it != in_flight_.end()) {
            for (id<MTLBuffer> b : it->second) {
                if (b.length == kChunkSize) {
                    free_chunks_.push_back(b);
                }
            }
            in_flight_.erase(it);
        }
        in_flight_retired_.erase(serial);
        uint64_t prev = completed_.load();
        while (serial > prev && !completed_.compare_exchange_weak(prev, serial)) {
        }
    }

    Staging stage(size_t size, size_t align) {
        ensure_cmd();
        NSUInteger aligned = (chunk_used_ + align - 1) & ~(static_cast<NSUInteger>(align) - 1);
        if (!current_chunk_ || aligned + size > current_chunk_.length) {
            if (size > kChunkSize) {
                id<MTLBuffer> big = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared];
                frame_chunks_.push_back(big);
                return {big, 0, static_cast<uint8_t*>(big.contents)};
            }
            id<MTLBuffer> chunk = nil;
            {
                std::lock_guard lock(pool_mutex_);
                if (!free_chunks_.empty()) {
                    chunk = free_chunks_.back();
                    free_chunks_.pop_back();
                }
            }
            if (!chunk) {
                chunk = [device_ newBufferWithLength:kChunkSize options:MTLResourceStorageModeShared];
            }
            frame_chunks_.push_back(chunk);
            current_chunk_ = chunk;
            aligned = 0;
        }
        chunk_used_ = aligned + size;
        return {current_chunk_, aligned, static_cast<uint8_t*>(current_chunk_.contents) + aligned};
    }

    std::pair<NSUInteger, NSUInteger> target_size() const {
        GpuTexture* t = color_.texture ? color_.texture : depth_;
        if (!t) {
            return {1, 1};
        }
        uint32_t level = color_.texture ? color_.level : 0;
        return {level_dim(t->info.width, level), level_dim(t->info.height, level)};
    }

    bool ensure_encoder() {
        if (enc_) {
            return true;
        }
        if (!color_.texture && !depth_) {
            return false;
        }
        ensure_cmd();
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        if (color_.texture) {
            rp.colorAttachments[0].texture = color_.texture->texture;
            rp.colorAttachments[0].level = color_.level;
            rp.colorAttachments[0].slice = color_.face;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            color_.texture->used_serial = serial_;
        }
        if (depth_) {
            rp.depthAttachment.texture = depth_->texture;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = depth_->texture;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            depth_->used_serial = serial_;
        }
        if (!color_.texture) {
            rp.renderTargetWidth = depth_->info.width;
            rp.renderTargetHeight = depth_->info.height;
        }
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
        current_pipeline_ = nil;
        return enc_ != nil;
    }

    void end_encoder() {
        if (enc_) {
            [enc_ endEncoding];
            enc_ = nil;
            current_pipeline_ = nil;
        }
    }

    id<MTLLibrary> library(const ShaderKey& key) {
        ShaderKey k = key;
        for (auto& e : k.elements) {
            e.offset = 0;
        }
        uint64_t h = hash_key(k);
        if (auto it = libraries_.find(h); it != libraries_.end()) {
            return it->second;
        }
        std::string src = generate_msl(k);
        NSError* error = nil;
        MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
        opts.mathMode = MTLMathModeSafe;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                                   options:opts
                                                     error:&error];
        if (!lib) {
            std::fprintf(stderr, "[METAL] shader compile failed: %s\n%s\n", error.localizedDescription.UTF8String,
                         src.c_str());
        }
        libraries_[h] = lib;
        return lib;
    }

    id<MTLRenderPipelineState> pipeline(const PipelineKey& key) {
        uint64_t h = hash_key(key);
        if (auto it = pipelines_.find(h); it != pipelines_.end()) {
            return it->second;
        }
        id<MTLLibrary> lib = library(key.shader);
        if (!lib) {
            pipelines_[h] = nil;
            return nil;
        }
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        d.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        std::array<uint32_t, kMaxStreams> extent{};
        std::array<bool, kMaxStreams> used{};
        for (unsigned i = 0; i < key.shader.element_count; ++i) {
            const auto& e = key.shader.elements[i];
            vd.attributes[i].format = vertex_format(e.type);
            vd.attributes[i].offset = e.offset;
            vd.attributes[i].bufferIndex = e.stream;
            used[e.stream] = true;
            extent[e.stream] = std::max<uint32_t>(extent[e.stream], e.offset + element_size(e.type));
        }
        for (unsigned s = 0; s < kMaxStreams; ++s) {
            if (!used[s]) {
                continue;
            }
            uint32_t stride = key.strides[s];
            if (stride == 0) {
                vd.layouts[s].stepFunction = MTLVertexStepFunctionConstant;
                vd.layouts[s].stepRate = 0;
                vd.layouts[s].stride = (extent[s] + 3) & ~3u;
            } else {
                vd.layouts[s].stride = stride;
            }
        }
        d.vertexDescriptor = vd;
        if (key.has_color) {
            auto* ca = d.colorAttachments[0];
            ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
            ca.writeMask = write_mask(key.write_mask);
            if (key.blend_enable) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = blend_factor(key.src_blend);
                ca.destinationRGBBlendFactor = blend_factor(key.dst_blend);
                ca.rgbBlendOperation = blend_operation(key.blend_op);
                ca.sourceAlphaBlendFactor = blend_factor(key.src_blend_alpha);
                ca.destinationAlphaBlendFactor = blend_factor(key.dst_blend_alpha);
                ca.alphaBlendOperation = blend_operation(key.blend_op_alpha);
            }
        }
        if (key.has_depth) {
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> ps = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
        if (!ps) {
            std::fprintf(stderr, "[METAL] pipeline failed: %s\n", error.localizedDescription.UTF8String);
        }
        pipelines_[h] = ps;
        return ps;
    }

    id<MTLRenderPipelineState> clear_pipeline(bool write_color) {
        uint32_t key = (color_.texture ? 1u : 0u) | (depth_ ? 2u : 0u) | (write_color ? 4u : 0u);
        if (auto it = clear_pipelines_.find(key); it != clear_pipelines_.end()) {
            return it->second;
        }
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = [utility_ newFunctionWithName:@"clear_vs"];
        d.fragmentFunction = [utility_ newFunctionWithName:@"clear_fs"];
        if (color_.texture) {
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            d.colorAttachments[0].writeMask = write_color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        if (depth_) {
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> ps = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
        if (!ps) {
            std::fprintf(stderr, "[METAL] clear pipeline failed: %s\n", error.localizedDescription.UTF8String);
        }
        clear_pipelines_[key] = ps;
        return ps;
    }

    id<MTLRenderPipelineState> quad_pipeline() {
        if (!quad_pipeline_) {
            MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
            d.vertexFunction = [utility_ newFunctionWithName:@"quad_vs"];
            d.fragmentFunction = [utility_ newFunctionWithName:@"quad_fs"];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            NSError* error = nil;
            quad_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
            if (!quad_pipeline_) {
                std::fprintf(stderr, "[METAL] quad pipeline failed: %s\n", error.localizedDescription.UTF8String);
            }
        }
        return quad_pipeline_;
    }

    id<MTLDepthStencilState> clear_depth_state(bool depth, bool stencil) {
        DepthStencilDesc d{};
        d.depth_test = depth ? 1 : 0;
        d.depth_write = depth ? 1 : 0;
        d.depth_func = d3d::cmp::always;
        if (stencil) {
            d.stencil = 1;
            d.front = StencilFace{3, 3, 3, d3d::cmp::always};
            d.back = d.front;
            d.write_mask = 0xff;
        }
        return depth_state(d);
    }

    id<MTLDepthStencilState> depth_state(const DepthStencilDesc& desc) {
        DepthStencilDesc k = desc;
        if (!depth_) {
            k = DepthStencilDesc{};
        }
        uint64_t h = hash_key(k);
        if (auto it = depth_states_.find(h); it != depth_states_.end()) {
            return it->second;
        }
        MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
        if (k.depth_test) {
            d.depthCompareFunction = compare(k.depth_func);
            d.depthWriteEnabled = k.depth_write != 0;
        } else {
            d.depthCompareFunction = MTLCompareFunctionAlways;
            d.depthWriteEnabled = NO;
        }
        if (k.stencil) {
            auto face = [&](const StencilFace& f) {
                MTLStencilDescriptor* s = [[MTLStencilDescriptor alloc] init];
                s.stencilFailureOperation = stencil_op(f.fail);
                s.depthFailureOperation = stencil_op(f.depth_fail);
                s.depthStencilPassOperation = stencil_op(f.pass);
                s.stencilCompareFunction = compare(f.func);
                s.readMask = k.read_mask;
                s.writeMask = k.write_mask;
                return s;
            };
            d.frontFaceStencil = face(k.front);
            d.backFaceStencil = face(k.back);
        }
        id<MTLDepthStencilState> st = [device_ newDepthStencilStateWithDescriptor:d];
        depth_states_[h] = st;
        return st;
    }

    id<MTLSamplerState> sampler(const SamplerDesc& desc, bool mipmapped) {
        SamplerDesc k = desc;
        if (!mipmapped) {
            k.mip = 0;
            k.max_mip_level = 0;
        }
        uint64_t h = hash_key(k);
        if (auto it = samplers_.find(h); it != samplers_.end()) {
            return it->second;
        }
        MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
        d.sAddressMode = address_mode(k.address_u);
        d.tAddressMode = address_mode(k.address_v);
        d.rAddressMode = address_mode(k.address_w);
        d.magFilter = k.mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = k.min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = k.mip == 0   ? MTLSamplerMipFilterNotMipmapped
                      : k.mip == 1 ? MTLSamplerMipFilterNearest
                                   : MTLSamplerMipFilterLinear;
        if (k.min == 3 || k.mag == 3) {
            d.maxAnisotropy = std::clamp<NSUInteger>(k.max_anisotropy, 1, 16);
        }
        d.borderColor = k.border_white ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorTransparentBlack;
        d.lodMinClamp = k.max_mip_level;
        id<MTLSamplerState> s = [device_ newSamplerStateWithDescriptor:d];
        samplers_[h] = s;
        return s;
    }

    PresentOptions options_;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLLibrary> utility_ = nil;
    CAMetalLayer* layer_ = nil;
    dispatch_semaphore_t frame_sem_;
    id<MTLCommandBuffer> cmd_ = nil;
    id<MTLCommandBuffer> last_cmd_ = nil;
    std::atomic<int> pending_captures_{0};
    id<MTLRenderCommandEncoder> enc_ = nil;
    id<MTLRenderPipelineState> current_pipeline_ = nil;
    id<MTLRenderPipelineState> quad_pipeline_ = nil;
    RenderTarget color_{};
    GpuTexture* depth_ = nullptr;
    uint64_t serial_ = 0;
    uint64_t serial_counter_ = 0;
    std::atomic<uint64_t> completed_{0};
    uint64_t frame_ = 0;
    uint64_t image_frame_ = 0;
    GpuTexture* image_ = nullptr;
    id<MTLBuffer> current_chunk_ = nil;
    NSUInteger chunk_used_ = 0;
    std::vector<id<MTLBuffer>> frame_chunks_;
    std::vector<id<MTLTexture>> retired_;
    std::mutex pool_mutex_;
    std::vector<id<MTLBuffer>> free_chunks_;
    std::unordered_map<uint64_t, std::vector<id<MTLBuffer>>> in_flight_;
    std::unordered_map<uint64_t, std::vector<id<MTLTexture>>> in_flight_retired_;
    std::unordered_map<uint64_t, id<MTLLibrary>> libraries_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines_;
    std::unordered_map<uint32_t, id<MTLRenderPipelineState>> clear_pipelines_;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
};

}

std::string compile_msl(const std::string& source) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            return "no Metal device";
        }
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                  options:nil
                                                    error:&error];
        if (!lib) {
            return error ? error.localizedDescription.UTF8String : "compile failed";
        }
        if (![lib newFunctionWithName:@"vs_main"] || ![lib newFunctionWithName:@"fs_main"]) {
            return "entry points missing";
        }
        return {};
    }
}

std::unique_ptr<Renderer> Renderer::create_metal(const PresentOptions& options) {
    return std::make_unique<MetalRenderer>(options);
}

}
