#include "d3d9.hpp"

#include "d3d9_defs.hpp"
#include "ff.hpp"
#include "formats.hpp"
#include "renderer.hpp"

#include "../input/keys.hpp"
#include "../platform/window.hpp"
#include "../settings.hpp"
#include "../win32/com_object.hpp"
#include "../win32/process.hpp"
#include "../win32/user32.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ma::host::d3d9 {

namespace {

using namespace win32;
using namespace gfx;
namespace D = gfx::d3d;

struct Device;

void log_once(const std::string& text) {
    static std::set<std::string> seen;
    if (seen.insert(text).second) {
        std::fprintf(stderr, "[D3D9] %s\n", text.c_str());
    }
}

float bitsf(uint32_t u) { return std::bit_cast<float>(u); }

struct Resource : ComObject {
    Device* device = nullptr;
    uint32_t device_guest = 0;
    uint32_t priority = 0;
};

struct Texture : Resource {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    uint32_t gpu_levels = 1;
    uint32_t usage = 0;
    uint32_t format = 0;
    uint32_t pool = 0;
    uint32_t faces = 1;
    uint32_t lod = 0;
    struct Level {
        uint32_t storage = 0;
        uint32_t pitch = 0;
        uint32_t size = 0;
    };
    std::vector<Level> mips;
    std::vector<uint32_t> surfaces;
    GpuTexture* gpu = nullptr;
    ~Texture() override;
    Level& level(uint32_t face, uint32_t l) { return mips[face * levels + l]; }
};

struct Surface : Resource {
    uint32_t container = 0;
    Texture* texture = nullptr;
    uint32_t face = 0;
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t usage = 0;
    uint32_t pool = 0;
    GpuTexture* gpu = nullptr;
    bool owns_gpu = false;
    uint32_t storage = 0;
    uint32_t pitch = 0;
    ~Surface() override;
};

struct Buffer : Resource {
    uint32_t storage = 0;
    uint32_t size = 0;
    uint32_t usage = 0;
    uint32_t format = 0;
    uint32_t pool = 0;
    bool index = false;
    ~Buffer() override { allocator().free(storage); }
};

struct VertexDecl : ComObject {
    std::vector<uint8_t> elements;
    VertexLayout layout;
    uint32_t device_guest = 0;
};

struct Shader : ComObject {
    std::vector<uint8_t> code;
    bool pixel = false;
    uint32_t device_guest = 0;
};

struct Query : ComObject {
    uint32_t type = 0;
    uint32_t device_guest = 0;
};

struct SwapChain : ComObject {
    uint32_t device_guest = 0;
};

struct Direct3D : ComObject {};

struct Stream {
    uint32_t buffer = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint32_t freq = 1;
};

struct Device : ComObject {
    uint32_t d3d = 0;
    uint32_t focus = 0;
    uint32_t window = 0;
    uint32_t behavior = 0;
    std::array<uint32_t, 14> present{};
    FixedFunctionState st{};
    uint32_t fvf = 0;
    bool use_decl = false;
    uint32_t decl = 0;
    std::array<Stream, kMaxStreams> streams{};
    uint32_t indices = 0;
    std::array<uint32_t, kMaxSamplers> textures{};
    uint32_t render_target = 0;
    uint32_t depth_stencil = 0;
    uint32_t backbuffer = 0;
    uint32_t auto_depth = 0;
    uint32_t swapchain = 0;
    uint32_t vertex_shader = 0;
    uint32_t pixel_shader = 0;
    std::vector<float> vs_const = std::vector<float>(256 * 4);
    std::vector<float> ps_const = std::vector<float>(224 * 4);
    std::array<int32_t, 64> vs_int{};
    std::array<int32_t, 64> ps_int{};
    std::array<uint32_t, 16> vs_bool{};
    std::array<uint32_t, 16> ps_bool{};
    std::array<float, 4 * 6> clip_planes{};
    uint32_t palette = 0;
    bool software_vp = false;
    float npatch = 0;
    std::unique_ptr<Renderer> renderer;
    uint64_t draws = 0;
    uint64_t skipped_shader_draws = 0;
};

Device* g_device = nullptr;

void release_guest(uint32_t guest) {
    if (guest != 0) {
        com_objects().release(guest);
    }
}

void add_ref_guest(uint32_t guest) {
    if (guest != 0) {
        com_objects().add_ref(guest);
    }
}

void rebind(uint32_t& slot, uint32_t value) {
    if (slot == value) {
        return;
    }
    add_ref_guest(value);
    uint32_t old = slot;
    slot = value;
    release_guest(old);
}

Texture::~Texture() {
    for (uint32_t s : surfaces) {
        if (s != 0) {
            com_objects().destroy(s);
        }
    }
    for (auto& m : mips) {
        allocator().free(m.storage);
    }
    if (gpu && g_device && g_device->renderer) {
        g_device->renderer->destroy_texture(gpu);
    }
}

Surface::~Surface() {
    allocator().free(storage);
    if (owns_gpu && gpu && g_device && g_device->renderer) {
        g_device->renderer->destroy_texture(gpu);
    }
}

uint32_t query_interface(uint32_t self, uint32_t riid, uint32_t out) {
    log_once("QueryInterface " + guid_string(riid) + " refused");
    com_out(out, 0);
    return kENoInterface;
}

uint32_t release_any(uint32_t self) { return com_obj_release(self); }

uint32_t surface_add_ref(uint32_t self) {
    auto* s = com_get<Surface>(self);
    if (s && s->container) {
        return com_obj_add_ref(s->container);
    }
    return com_obj_add_ref(self);
}

uint32_t surface_release(uint32_t self) {
    auto* s = com_get<Surface>(self);
    if (s && s->container) {
        return com_obj_release(s->container);
    }
    return com_obj_release(self);
}

uint32_t resource_get_device(uint32_t self, uint32_t out) {
    auto* r = com_get<Resource>(self);
    if (!r) {
        return D::kInvalidCall;
    }
    add_ref_guest(r->device_guest);
    com_out(out, r->device_guest);
    return D::kOk;
}

uint32_t resource_set_priority(uint32_t self, uint32_t p) {
    auto* r = com_get<Resource>(self);
    if (!r) {
        return 0;
    }
    uint32_t old = r->priority;
    r->priority = p;
    return old;
}

uint32_t resource_get_priority(uint32_t self) {
    auto* r = com_get<Resource>(self);
    return r ? r->priority : 0;
}

uint32_t resource_preload(uint32_t self) { return 0; }

uint32_t texture_get_type(uint32_t self) {
    auto* t = com_get<Texture>(self);
    return t && t->faces == 6 ? D::rtype::cube_texture : D::rtype::texture;
}

uint32_t surface_get_type(uint32_t self) { return D::rtype::surface; }

uint32_t buffer_get_type(uint32_t self) {
    auto* b = com_get<Buffer>(self);
    return b && b->index ? D::rtype::index_buffer : D::rtype::vertex_buffer;
}

void upload_level(Texture& t, uint32_t face, uint32_t l) {
    if (!t.gpu || !g_device || !g_device->renderer) {
        return;
    }
    Texture::Level& lv = t.level(face, l);
    if (lv.storage == 0) {
        return;
    }
    uint32_t w = level_dim(t.width, l);
    uint32_t h = level_dim(t.height, l);
    if (needs_conversion(t.format)) {
        std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
        convert_to_bgra8(t.format, mem::ptr(lv.storage), lv.pitch, w, h, tmp.data(), w * 4);
        g_device->renderer->upload(t.gpu, face, l, tmp.data(), w * 4);
    } else {
        g_device->renderer->upload(t.gpu, face, l, mem::ptr(lv.storage), lv.pitch);
    }
    if (l == 0 && (t.usage & D::usage::autogen_mipmap) && t.gpu_levels > 1) {
        g_device->renderer->generate_mipmaps(t.gpu);
    }
}

void fill_surface_desc(uint32_t out, uint32_t format, uint32_t usage, uint32_t pool, uint32_t w, uint32_t h) {
    mem::write<uint32_t>(out, format);
    mem::write<uint32_t>(out + 4, D::rtype::surface);
    mem::write<uint32_t>(out + 8, usage);
    mem::write<uint32_t>(out + 12, pool);
    mem::write<uint32_t>(out + 16, 0);
    mem::write<uint32_t>(out + 20, 0);
    mem::write<uint32_t>(out + 24, w);
    mem::write<uint32_t>(out + 28, h);
}

uint32_t lock_rect(Texture& t, uint32_t face, uint32_t l, uint32_t out, uint32_t rect, uint32_t flags) {
    if (l >= t.levels || out == 0) {
        return D::kInvalidCall;
    }
    Texture::Level& lv = t.level(face, l);
    if (lv.storage == 0) {
        uint32_t rows = level_rows(t.format, level_dim(t.height, l));
        lv.pitch = level_pitch(t.format, level_dim(t.width, l));
        lv.size = lv.pitch * rows;
        lv.storage = allocator().alloc(lv.size);
        if (lv.storage == 0) {
            return D::kOutOfVideoMemory;
        }
    }
    uint32_t bits = lv.storage;
    if (rect != 0) {
        int32_t left = mem::read<int32_t>(rect);
        int32_t top = mem::read<int32_t>(rect + 4);
        FormatInfo fi = format_info(t.format);
        if (fi.compressed) {
            bits += static_cast<uint32_t>(top / 4) * lv.pitch + static_cast<uint32_t>(left / 4) * fi.bytes;
        } else {
            bits += static_cast<uint32_t>(top) * lv.pitch + static_cast<uint32_t>(left) * fi.bytes;
        }
    }
    mem::write<uint32_t>(out, lv.pitch);
    mem::write<uint32_t>(out + 4, bits);
    return D::kOk;
}

uint32_t create_level_surface(Texture& t, uint32_t face, uint32_t l);

uint32_t tex_set_lod(uint32_t self, uint32_t lod) {
    auto* t = com_get<Texture>(self);
    if (!t) {
        return 0;
    }
    uint32_t old = t->lod;
    t->lod = std::min(lod, t->levels - 1);
    return old;
}

uint32_t tex_get_lod(uint32_t self) {
    auto* t = com_get<Texture>(self);
    return t ? t->lod : 0;
}

uint32_t tex_get_level_count(uint32_t self) {
    auto* t = com_get<Texture>(self);
    return t ? t->levels : 0;
}

uint32_t tex_set_autogen_filter(uint32_t self, uint32_t filter) { return D::kOk; }
uint32_t tex_get_autogen_filter(uint32_t self) { return 2; }

uint32_t tex_generate_mips(uint32_t self) {
    auto* t = com_get<Texture>(self);
    if (t && t->gpu && g_device && g_device->renderer) {
        g_device->renderer->generate_mipmaps(t->gpu);
    }
    return 0;
}

uint32_t tex_get_level_desc(uint32_t self, uint32_t l, uint32_t out) {
    auto* t = com_get<Texture>(self);
    if (!t || l >= t->levels || out == 0) {
        return D::kInvalidCall;
    }
    fill_surface_desc(out, t->format, t->usage, t->pool, level_dim(t->width, l), level_dim(t->height, l));
    return D::kOk;
}

uint32_t tex_get_surface_level(uint32_t self, uint32_t l, uint32_t out) {
    auto* t = com_get<Texture>(self);
    if (!t || l >= t->levels || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t s = create_level_surface(*t, 0, l);
    com_obj_add_ref(self);
    com_out(out, s);
    return D::kOk;
}

uint32_t tex_lock_rect(uint32_t self, uint32_t l, uint32_t out, uint32_t rect, uint32_t flags) {
    auto* t = com_get<Texture>(self);
    return t ? lock_rect(*t, 0, l, out, rect, flags) : D::kInvalidCall;
}

uint32_t tex_unlock_rect(uint32_t self, uint32_t l) {
    auto* t = com_get<Texture>(self);
    if (!t || l >= t->levels) {
        return D::kInvalidCall;
    }
    upload_level(*t, 0, l);
    return D::kOk;
}

uint32_t tex_add_dirty_rect(uint32_t self, uint32_t rect) { return D::kOk; }

uint32_t cube_get_level_desc(uint32_t self, uint32_t l, uint32_t out) { return tex_get_level_desc(self, l, out); }

uint32_t cube_get_surface(uint32_t self, uint32_t face, uint32_t l, uint32_t out) {
    auto* t = com_get<Texture>(self);
    if (!t || face >= t->faces || l >= t->levels || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t s = create_level_surface(*t, face, l);
    com_obj_add_ref(self);
    com_out(out, s);
    return D::kOk;
}

uint32_t cube_lock_rect(uint32_t self, uint32_t face, uint32_t l, uint32_t out, uint32_t rect, uint32_t flags) {
    auto* t = com_get<Texture>(self);
    return t && face < t->faces ? lock_rect(*t, face, l, out, rect, flags) : D::kInvalidCall;
}

uint32_t cube_unlock_rect(uint32_t self, uint32_t face, uint32_t l) {
    auto* t = com_get<Texture>(self);
    if (!t || face >= t->faces || l >= t->levels) {
        return D::kInvalidCall;
    }
    upload_level(*t, face, l);
    return D::kOk;
}

uint32_t cube_add_dirty_rect(uint32_t self, uint32_t face, uint32_t rect) { return D::kOk; }

#define RESOURCE_METHODS(ADDREF, RELEASE, GETTYPE)                                                                     \
    com_method<query_interface>("QueryInterface"), com_method<ADDREF>("AddRef"), com_method<RELEASE>("Release"),       \
        com_method<resource_get_device>("GetDevice"), ComMethod{"SetPrivateData", 5},                                  \
        ComMethod{"GetPrivateData", 4, nullptr, D::kNotFound}, ComMethod{"FreePrivateData", 2, nullptr, D::kNotFound}, \
        com_method<resource_set_priority>("SetPriority"), com_method<resource_get_priority>("GetPriority"),            \
        com_method<resource_preload>("PreLoad"), com_method<GETTYPE>("GetType")

ComInterface g_texture_iface("d3d9.dll", "IDirect3DTexture9",
                             {
                                 RESOURCE_METHODS(com_obj_add_ref, release_any, texture_get_type),
                                 com_method<tex_set_lod>("SetLOD"),
                                 com_method<tex_get_lod>("GetLOD"),
                                 com_method<tex_get_level_count>("GetLevelCount"),
                                 com_method<tex_set_autogen_filter>("SetAutoGenFilterType"),
                                 com_method<tex_get_autogen_filter>("GetAutoGenFilterType"),
                                 com_method<tex_generate_mips>("GenerateMipSubLevels"),
                                 com_method<tex_get_level_desc>("GetLevelDesc"),
                                 com_method<tex_get_surface_level>("GetSurfaceLevel"),
                                 com_method<tex_lock_rect>("LockRect"),
                                 com_method<tex_unlock_rect>("UnlockRect"),
                                 com_method<tex_add_dirty_rect>("AddDirtyRect"),
                             });

ComInterface g_cube_iface("d3d9.dll", "IDirect3DCubeTexture9",
                          {
                              RESOURCE_METHODS(com_obj_add_ref, release_any, texture_get_type),
                              com_method<tex_set_lod>("SetLOD"),
                              com_method<tex_get_lod>("GetLOD"),
                              com_method<tex_get_level_count>("GetLevelCount"),
                              com_method<tex_set_autogen_filter>("SetAutoGenFilterType"),
                              com_method<tex_get_autogen_filter>("GetAutoGenFilterType"),
                              com_method<tex_generate_mips>("GenerateMipSubLevels"),
                              com_method<cube_get_level_desc>("GetLevelDesc"),
                              com_method<cube_get_surface>("GetCubeMapSurface"),
                              com_method<cube_lock_rect>("LockRect"),
                              com_method<cube_unlock_rect>("UnlockRect"),
                              com_method<cube_add_dirty_rect>("AddDirtyRect"),
                          });

RenderTarget target_of(Surface& s) {
    if (s.texture) {
        return RenderTarget{s.texture->gpu, s.face, s.level};
    }
    return RenderTarget{s.gpu, 0, 0};
}

uint32_t surf_get_container(uint32_t self, uint32_t riid, uint32_t out) {
    auto* s = com_get<Surface>(self);
    if (!s || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t c = s->container ? s->container : s->device_guest;
    add_ref_guest(c);
    com_out(out, c);
    return D::kOk;
}

uint32_t surf_get_desc(uint32_t self, uint32_t out) {
    auto* s = com_get<Surface>(self);
    if (!s || out == 0) {
        return D::kInvalidCall;
    }
    fill_surface_desc(out, s->format, s->usage, s->pool, s->width, s->height);
    return D::kOk;
}

uint32_t surf_lock_rect(uint32_t self, uint32_t out, uint32_t rect, uint32_t flags) {
    auto* s = com_get<Surface>(self);
    if (!s || out == 0) {
        return D::kInvalidCall;
    }
    if (s->texture) {
        return lock_rect(*s->texture, s->face, s->level, out, rect, flags);
    }
    if (s->storage == 0) {
        s->pitch = level_pitch(s->format, s->width);
        s->storage = allocator().alloc(s->pitch * level_rows(s->format, s->height));
        if (s->storage == 0) {
            return D::kOutOfVideoMemory;
        }
    }
    if (s->gpu && format_info(s->format).render_target && g_device && g_device->renderer) {
        g_device->renderer->read_back(RenderTarget{s->gpu, 0, 0}, mem::ptr(s->storage), s->pitch);
    }
    uint32_t bits = s->storage;
    if (rect != 0) {
        FormatInfo fi = format_info(s->format);
        bits += static_cast<uint32_t>(mem::read<int32_t>(rect + 4)) * s->pitch +
                static_cast<uint32_t>(mem::read<int32_t>(rect)) * fi.bytes;
    }
    mem::write<uint32_t>(out, s->pitch);
    mem::write<uint32_t>(out + 4, bits);
    return D::kOk;
}

uint32_t surf_unlock_rect(uint32_t self) {
    auto* s = com_get<Surface>(self);
    if (!s) {
        return D::kInvalidCall;
    }
    if (s->texture) {
        upload_level(*s->texture, s->face, s->level);
    }
    return D::kOk;
}

ComInterface g_surface_iface("d3d9.dll", "IDirect3DSurface9",
                             {
                                 RESOURCE_METHODS(surface_add_ref, surface_release, surface_get_type),
                                 com_method<surf_get_container>("GetContainer"),
                                 com_method<surf_get_desc>("GetDesc"),
                                 com_method<surf_lock_rect>("LockRect"),
                                 com_method<surf_unlock_rect>("UnlockRect"),
                                 ComMethod{"GetDC", 2, nullptr, D::kInvalidCall},
                                 ComMethod{"ReleaseDC", 2, nullptr, D::kInvalidCall},
                             });

uint32_t create_level_surface(Texture& t, uint32_t face, uint32_t l) {
    uint32_t& slot = t.surfaces[face * t.levels + l];
    if (slot == 0) {
        auto s = std::make_unique<Surface>();
        s->device = t.device;
        s->device_guest = t.device_guest;
        s->container = t.guest;
        s->texture = &t;
        s->face = face;
        s->level = l;
        s->width = level_dim(t.width, l);
        s->height = level_dim(t.height, l);
        s->format = t.format;
        s->usage = t.usage;
        s->pool = t.pool;
        slot = com_objects().wrap(g_surface_iface, std::move(s));
    }
    return slot;
}

uint32_t buf_lock(uint32_t self, uint32_t offset, uint32_t size, uint32_t out, uint32_t flags) {
    auto* b = com_get<Buffer>(self);
    if (!b || out == 0 || offset > b->size) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, b->storage + offset);
    return D::kOk;
}

uint32_t buf_unlock(uint32_t self) { return D::kOk; }

uint32_t vb_get_desc(uint32_t self, uint32_t out) {
    auto* b = com_get<Buffer>(self);
    if (!b || out == 0) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, D::fmt::vertexdata);
    mem::write<uint32_t>(out + 4, D::rtype::vertex_buffer);
    mem::write<uint32_t>(out + 8, b->usage);
    mem::write<uint32_t>(out + 12, b->pool);
    mem::write<uint32_t>(out + 16, b->size);
    mem::write<uint32_t>(out + 20, b->format);
    return D::kOk;
}

uint32_t ib_get_desc(uint32_t self, uint32_t out) {
    auto* b = com_get<Buffer>(self);
    if (!b || out == 0) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, b->format);
    mem::write<uint32_t>(out + 4, D::rtype::index_buffer);
    mem::write<uint32_t>(out + 8, b->usage);
    mem::write<uint32_t>(out + 12, b->pool);
    mem::write<uint32_t>(out + 16, b->size);
    return D::kOk;
}

ComInterface g_vb_iface("d3d9.dll", "IDirect3DVertexBuffer9",
                        {
                            RESOURCE_METHODS(com_obj_add_ref, release_any, buffer_get_type),
                            com_method<buf_lock>("Lock"),
                            com_method<buf_unlock>("Unlock"),
                            com_method<vb_get_desc>("GetDesc"),
                        });

ComInterface g_ib_iface("d3d9.dll", "IDirect3DIndexBuffer9",
                        {
                            RESOURCE_METHODS(com_obj_add_ref, release_any, buffer_get_type),
                            com_method<buf_lock>("Lock"),
                            com_method<buf_unlock>("Unlock"),
                            com_method<ib_get_desc>("GetDesc"),
                        });

uint32_t child_get_device(uint32_t self, uint32_t out) {
    uint32_t dev = 0;
    if (auto* d = com_get<VertexDecl>(self)) {
        dev = d->device_guest;
    } else if (auto* s = com_get<Shader>(self)) {
        dev = s->device_guest;
    } else if (auto* q = com_get<Query>(self)) {
        dev = q->device_guest;
    } else if (auto* sc = com_get<SwapChain>(self)) {
        dev = sc->device_guest;
    }
    add_ref_guest(dev);
    com_out(out, dev);
    return dev ? D::kOk : D::kInvalidCall;
}

uint32_t decl_get_declaration(uint32_t self, uint32_t out, uint32_t count) {
    auto* d = com_get<VertexDecl>(self);
    if (!d || count == 0) {
        return D::kInvalidCall;
    }
    uint32_t n = static_cast<uint32_t>(d->elements.size() / 8);
    if (out != 0) {
        std::memcpy(mem::ptr(out), d->elements.data(), d->elements.size());
    }
    mem::write<uint32_t>(count, n);
    return D::kOk;
}

ComInterface g_decl_iface("d3d9.dll", "IDirect3DVertexDeclaration9",
                          {
                              com_method<query_interface>("QueryInterface"),
                              com_method<com_obj_add_ref>("AddRef"),
                              com_method<release_any>("Release"),
                              com_method<child_get_device>("GetDevice"),
                              com_method<decl_get_declaration>("GetDeclaration"),
                          });

uint32_t shader_get_function(uint32_t self, uint32_t out, uint32_t size) {
    auto* s = com_get<Shader>(self);
    if (!s || size == 0) {
        return D::kInvalidCall;
    }
    if (out != 0) {
        std::memcpy(mem::ptr(out), s->code.data(), std::min<size_t>(s->code.size(), mem::read<uint32_t>(size)));
    }
    mem::write<uint32_t>(size, static_cast<uint32_t>(s->code.size()));
    return D::kOk;
}

ComInterface g_vshader_iface("d3d9.dll", "IDirect3DVertexShader9",
                             {
                                 com_method<query_interface>("QueryInterface"),
                                 com_method<com_obj_add_ref>("AddRef"),
                                 com_method<release_any>("Release"),
                                 com_method<child_get_device>("GetDevice"),
                                 com_method<shader_get_function>("GetFunction"),
                             });

ComInterface g_pshader_iface("d3d9.dll", "IDirect3DPixelShader9",
                             {
                                 com_method<query_interface>("QueryInterface"),
                                 com_method<com_obj_add_ref>("AddRef"),
                                 com_method<release_any>("Release"),
                                 com_method<child_get_device>("GetDevice"),
                                 com_method<shader_get_function>("GetFunction"),
                             });

uint32_t query_get_type(uint32_t self) {
    auto* q = com_get<Query>(self);
    return q ? q->type : 0;
}

uint32_t query_get_data_size(uint32_t self) { return 4; }
uint32_t query_issue(uint32_t self, uint32_t flags) { return D::kOk; }

uint32_t query_get_data(uint32_t self, uint32_t data, uint32_t size, uint32_t flags) {
    auto* q = com_get<Query>(self);
    if (!q) {
        return D::kInvalidCall;
    }
    if (data != 0 && size >= 4) {
        mem::write<uint32_t>(data, q->type == 9 ? 1000000u : 1u);
    }
    return D::kOk;
}

ComInterface g_query_iface("d3d9.dll", "IDirect3DQuery9",
                           {
                               com_method<query_interface>("QueryInterface"),
                               com_method<com_obj_add_ref>("AddRef"),
                               com_method<release_any>("Release"),
                               com_method<child_get_device>("GetDevice"),
                               com_method<query_get_type>("GetType"),
                               com_method<query_get_data_size>("GetDataSize"),
                               com_method<query_issue>("Issue"),
                               com_method<query_get_data>("GetData"),
                           });

uint32_t dev_present(uint32_t self, uint32_t src, uint32_t dst, uint32_t hwnd, uint32_t dirty);
uint32_t dev_get_back_buffer(uint32_t self, uint32_t chain, uint32_t index, uint32_t type, uint32_t out);
uint32_t dev_get_display_mode(uint32_t self, uint32_t chain, uint32_t out);

uint32_t sc_present(uint32_t self, uint32_t src, uint32_t dst, uint32_t hwnd, uint32_t dirty, uint32_t flags) {
    auto* sc = com_get<SwapChain>(self);
    return sc ? dev_present(sc->device_guest, src, dst, hwnd, dirty) : D::kInvalidCall;
}

uint32_t sc_get_back_buffer(uint32_t self, uint32_t index, uint32_t type, uint32_t out) {
    auto* sc = com_get<SwapChain>(self);
    return sc ? dev_get_back_buffer(sc->device_guest, 0, index, type, out) : D::kInvalidCall;
}

uint32_t sc_get_display_mode(uint32_t self, uint32_t out) {
    auto* sc = com_get<SwapChain>(self);
    return sc ? dev_get_display_mode(sc->device_guest, 0, out) : D::kInvalidCall;
}

uint32_t sc_get_present_parameters(uint32_t self, uint32_t out) {
    auto* sc = com_get<SwapChain>(self);
    auto* d = sc ? com_get<Device>(sc->device_guest) : nullptr;
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(out), d->present.data(), sizeof(d->present));
    return D::kOk;
}

uint32_t sc_get_raster_status(uint32_t self, uint32_t out) {
    if (out != 0) {
        mem::write<uint32_t>(out, 0);
        mem::write<uint32_t>(out + 4, 0);
    }
    return D::kOk;
}

ComInterface g_swapchain_iface("d3d9.dll", "IDirect3DSwapChain9",
                               {
                                   com_method<query_interface>("QueryInterface"),
                                   com_method<com_obj_add_ref>("AddRef"),
                                   com_method<release_any>("Release"),
                                   com_method<sc_present>("Present"),
                                   ComMethod{"GetFrontBufferData", 2, nullptr, D::kInvalidCall},
                                   com_method<sc_get_back_buffer>("GetBackBuffer"),
                                   com_method<sc_get_raster_status>("GetRasterStatus"),
                                   com_method<sc_get_display_mode>("GetDisplayMode"),
                                   com_method<child_get_device>("GetDevice"),
                                   com_method<sc_get_present_parameters>("GetPresentParameters"),
                               });

Device* dev(uint32_t self) { return com_get<Device>(self); }

uint32_t primitive_vertices(uint8_t type, uint32_t count) {
    switch (type) {
    case D::prim::pointlist: return count;
    case D::prim::linelist: return count * 2;
    case D::prim::linestrip: return count + 1;
    case D::prim::trianglelist: return count * 3;
    case D::prim::trianglestrip:
    case D::prim::trianglefan: return count + 2;
    default: return 0;
    }
}

const VertexLayout& current_layout(Device& d) {
    static VertexLayout empty;
    static thread_local uint32_t cached_fvf = 0xffffffffu;
    static thread_local VertexLayout cached;
    if (d.use_decl) {
        auto* decl = com_get<VertexDecl>(d.decl);
        return decl ? decl->layout : empty;
    }
    if (cached_fvf != d.fvf) {
        cached = layout_from_fvf(d.fvf);
        cached_fvf = d.fvf;
    }
    return cached;
}

SamplerDesc sampler_desc(const std::array<uint32_t, D::samp::count>& s) {
    SamplerDesc d{};
    d.address_u = static_cast<uint8_t>(s[D::samp::addressu]);
    d.address_v = static_cast<uint8_t>(s[D::samp::addressv]);
    d.address_w = static_cast<uint8_t>(s[D::samp::addressw]);
    d.mag = static_cast<uint8_t>(s[D::samp::magfilter]);
    d.min = static_cast<uint8_t>(s[D::samp::minfilter]);
    d.mip = static_cast<uint8_t>(s[D::samp::mipfilter]);
    d.max_anisotropy = static_cast<uint8_t>(std::clamp<uint32_t>(s[D::samp::maxanisotropy], 1, 16));
    uint32_t border = s[D::samp::bordercolor];
    d.border_white = ((border >> 24) != 0 && (border & 0xffffff) == 0xffffff) ? 1 : 0;
    d.max_mip_level = static_cast<uint8_t>(std::min<uint32_t>(s[D::samp::maxmiplevel], 15));
    return d;
}

bool prepare_draw(Device& d, DrawCall& call, const VertexLayout& layout,
                  std::span<const uint16_t, kMaxStreams> strides) {
    if (d.vertex_shader != 0 || d.pixel_shader != 0) {
        ++d.skipped_shader_draws;
        log_once("draw with a programmable shader skipped (shader translator is M4 work)");
        return false;
    }
    auto* rt = com_get<Surface>(d.render_target);
    auto* ds = com_get<Surface>(d.depth_stencil);
    RenderTarget color = rt ? target_of(*rt) : RenderTarget{};
    GpuTexture* depth = ds ? ds->gpu : nullptr;
    d.renderer->set_targets(color, depth);
    std::array<TextureKind, kMaxStages> kinds{};
    for (unsigned i = 0; i < kMaxStages; ++i) {
        auto* t = com_get<Texture>(d.textures[i]);
        if (t && t->gpu) {
            kinds[i] = t->faces == 6 ? TextureKind::cube : TextureKind::tex2d;
            call.textures[i] = t->gpu;
            call.samplers[i] = sampler_desc(d.st.ss[i]);
        }
    }
    call.key = build_pipeline_key(d.st, layout, strides, kinds, color.texture != nullptr, depth != nullptr);
    build_uniforms(d.st, call.vs, call.fs);
    call.viewport = d.st.viewport;
    call.scissor_enable = d.st.rs[D::rs::scissortestenable] != 0;
    call.scissor = d.st.scissor;
    const auto& rs = d.st.rs;
    call.depth_stencil.depth_test = rs[D::rs::zenable] != 0 ? 1 : 0;
    call.depth_stencil.depth_write = rs[D::rs::zwriteenable] != 0 ? 1 : 0;
    call.depth_stencil.depth_func = static_cast<uint8_t>(rs[D::rs::zfunc]);
    if (rs[D::rs::stencilenable]) {
        call.depth_stencil.stencil = 1;
        call.depth_stencil.front = StencilFace{static_cast<uint8_t>(rs[D::rs::stencilfail]),
                                               static_cast<uint8_t>(rs[D::rs::stencilzfail]),
                                               static_cast<uint8_t>(rs[D::rs::stencilpass]),
                                               static_cast<uint8_t>(rs[D::rs::stencilfunc])};
        call.depth_stencil.back = rs[D::rs::twosidedstencilmode]
                                      ? StencilFace{static_cast<uint8_t>(rs[D::rs::ccw_stencilfail]),
                                                    static_cast<uint8_t>(rs[D::rs::ccw_stencilzfail]),
                                                    static_cast<uint8_t>(rs[D::rs::ccw_stencilpass]),
                                                    static_cast<uint8_t>(rs[D::rs::ccw_stencilfunc])}
                                      : call.depth_stencil.front;
        call.depth_stencil.read_mask = static_cast<uint8_t>(rs[D::rs::stencilmask]);
        call.depth_stencil.write_mask = static_cast<uint8_t>(rs[D::rs::stencilwritemask]);
    }
    call.cull = static_cast<uint8_t>(rs[D::rs::cullmode]);
    call.wireframe = rs[D::rs::fillmode] == 2;
    call.depth_bias = bitsf(rs[D::rs::depthbias]) * 16777216.0f;
    call.slope_bias = bitsf(rs[D::rs::slopescaledepthbias]);
    call.stencil_ref = rs[D::rs::stencilref];
    color_to_float4(rs[D::rs::blendfactor], call.blend_color);
    ++d.draws;
    return true;
}

std::array<uint16_t, kMaxStreams> stream_strides(Device& d) {
    std::array<uint16_t, kMaxStreams> s{};
    for (unsigned i = 0; i < kMaxStreams; ++i) {
        s[i] = static_cast<uint16_t>(d.streams[i].stride);
    }
    return s;
}

uint32_t layout_extent(const VertexLayout& l, unsigned stream) {
    uint32_t end = 0;
    for (unsigned i = 0; i < l.count; ++i) {
        if (l.elements[i].stream == stream) {
            end = std::max<uint32_t>(end, l.elements[i].offset + element_size(l.elements[i].type));
        }
    }
    return end;
}

bool gather_streams(Device& d, const VertexLayout& layout, uint32_t first, uint32_t count, DrawCall& call) {
    for (unsigned i = 0; i < layout.count; ++i) {
        unsigned s = layout.elements[i].stream;
        if (call.streams[s].data) {
            continue;
        }
        auto* b = com_get<Buffer>(d.streams[s].buffer);
        if (!b) {
            log_once("draw skipped: no vertex buffer bound to a used stream");
            return false;
        }
        uint32_t stride = d.streams[s].stride;
        uint64_t start = static_cast<uint64_t>(d.streams[s].offset) + static_cast<uint64_t>(first) * stride;
        uint64_t size = stride ? static_cast<uint64_t>(count) * stride : layout_extent(layout, s);
        if (start + size > b->size) {
            log_once("draw clamped: vertex range exceeds the bound buffer");
            if (start >= b->size) {
                return false;
            }
            size = b->size - start;
        }
        call.streams[s] = StreamData{mem::ptr(b->storage + static_cast<uint32_t>(start)), static_cast<uint32_t>(size)};
    }
    return true;
}

template <typename T>
void rebase_indices(const uint8_t* src, uint32_t count, std::vector<uint8_t>& out, uint32_t& min_index,
                    uint32_t& max_index) {
    min_index = 0xffffffffu;
    max_index = 0;
    for (uint32_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        min_index = std::min<uint32_t>(min_index, v);
        max_index = std::max<uint32_t>(max_index, v);
    }
    if (count == 0) {
        min_index = 0;
    }
    out.resize(static_cast<size_t>(count) * sizeof(T));
    for (uint32_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        v = static_cast<T>(v - min_index);
        std::memcpy(out.data() + i * sizeof(T), &v, sizeof(T));
    }
}

uint32_t dev_draw_primitive(uint32_t self, uint32_t type, uint32_t start, uint32_t count) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    const VertexLayout& layout = current_layout(*d);
    uint32_t n = primitive_vertices(static_cast<uint8_t>(type), count);
    if (n == 0) {
        return D::kOk;
    }
    auto call = std::make_unique<DrawCall>();
    auto strides = stream_strides(*d);
    if (!prepare_draw(*d, *call, layout, strides) || !gather_streams(*d, layout, start, n, *call)) {
        return D::kOk;
    }
    call->primitive = static_cast<uint8_t>(type);
    call->vertex_count = n;
    d->renderer->draw(*call);
    return D::kOk;
}

uint32_t dev_draw_indexed_primitive(uint32_t self, uint32_t type, int32_t base, uint32_t min_index, uint32_t num,
                                    uint32_t start_index, uint32_t count) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    auto* ib = com_get<Buffer>(d->indices);
    if (!ib) {
        log_once("DrawIndexedPrimitive without an index buffer");
        return D::kInvalidCall;
    }
    uint32_t n = primitive_vertices(static_cast<uint8_t>(type), count);
    if (n == 0) {
        return D::kOk;
    }
    bool idx32 = ib->format == D::fmt::index32;
    uint32_t isize = idx32 ? 4 : 2;
    if (static_cast<uint64_t>(start_index + n) * isize > ib->size) {
        log_once("DrawIndexedPrimitive index range exceeds the index buffer");
        return D::kInvalidCall;
    }
    std::vector<uint8_t> rebased;
    uint32_t lo = 0;
    uint32_t hi = 0;
    const uint8_t* src = mem::ptr(ib->storage + start_index * isize);
    if (idx32) {
        rebase_indices<uint32_t>(src, n, rebased, lo, hi);
    } else {
        rebase_indices<uint16_t>(src, n, rebased, lo, hi);
    }
    const VertexLayout& layout = current_layout(*d);
    auto call = std::make_unique<DrawCall>();
    auto strides = stream_strides(*d);
    int64_t first = static_cast<int64_t>(base) + lo;
    if (first < 0 || !prepare_draw(*d, *call, layout, strides) ||
        !gather_streams(*d, layout, static_cast<uint32_t>(first), hi - lo + 1, *call)) {
        return D::kOk;
    }
    call->primitive = static_cast<uint8_t>(type);
    call->indices = rebased.data();
    call->index_count = n;
    call->index32 = idx32;
    call->vertex_count = hi - lo + 1;
    d->renderer->draw(*call);
    return D::kOk;
}

void reset_stream_zero(Device& d) {
    rebind(d.streams[0].buffer, 0);
    d.streams[0].offset = 0;
    d.streams[0].stride = 0;
    rebind(d.indices, 0);
}

uint32_t dev_draw_primitive_up(uint32_t self, uint32_t type, uint32_t count, uint32_t data, uint32_t stride) {
    Device* d = dev(self);
    if (!d || data == 0) {
        return D::kInvalidCall;
    }
    reset_stream_zero(*d);
    uint32_t n = primitive_vertices(static_cast<uint8_t>(type), count);
    if (n == 0) {
        return D::kOk;
    }
    const VertexLayout& layout = current_layout(*d);
    auto call = std::make_unique<DrawCall>();
    std::array<uint16_t, kMaxStreams> strides{};
    strides[0] = static_cast<uint16_t>(stride);
    if (!prepare_draw(*d, *call, layout, strides)) {
        return D::kOk;
    }
    call->streams[0] = StreamData{mem::ptr(data), n * stride};
    call->primitive = static_cast<uint8_t>(type);
    call->vertex_count = n;
    d->renderer->draw(*call);
    return D::kOk;
}

uint32_t dev_draw_indexed_primitive_up(uint32_t self, uint32_t type, uint32_t min_index, uint32_t num,
                                       uint32_t count, uint32_t index_data, uint32_t index_format, uint32_t data,
                                       uint32_t stride) {
    Device* d = dev(self);
    if (!d || data == 0 || index_data == 0) {
        return D::kInvalidCall;
    }
    reset_stream_zero(*d);
    uint32_t n = primitive_vertices(static_cast<uint8_t>(type), count);
    if (n == 0) {
        return D::kOk;
    }
    bool idx32 = index_format == D::fmt::index32;
    std::vector<uint8_t> rebased;
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (idx32) {
        rebase_indices<uint32_t>(mem::ptr(index_data), n, rebased, lo, hi);
    } else {
        rebase_indices<uint16_t>(mem::ptr(index_data), n, rebased, lo, hi);
    }
    const VertexLayout& layout = current_layout(*d);
    auto call = std::make_unique<DrawCall>();
    std::array<uint16_t, kMaxStreams> strides{};
    strides[0] = static_cast<uint16_t>(stride);
    if (!prepare_draw(*d, *call, layout, strides)) {
        return D::kOk;
    }
    call->streams[0] = StreamData{mem::ptr(data + lo * stride), (hi - lo + 1) * stride};
    call->primitive = static_cast<uint8_t>(type);
    call->indices = rebased.data();
    call->index_count = n;
    call->index32 = idx32;
    call->vertex_count = hi - lo + 1;
    d->renderer->draw(*call);
    return D::kOk;
}

uint32_t dev_test_cooperative_level(uint32_t self) { return D::kOk; }
uint32_t dev_get_available_texture_mem(uint32_t self) { return 0x20000000u; }
uint32_t dev_evict_managed(uint32_t self) { return D::kOk; }

uint32_t dev_get_direct3d(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->d3d);
    com_out(out, d->d3d);
    return D::kOk;
}

void fill_caps(uint32_t p);

uint32_t dev_get_device_caps(uint32_t self, uint32_t out) {
    if (out == 0) {
        return D::kInvalidCall;
    }
    fill_caps(out);
    return D::kOk;
}

void write_mode(uint32_t out) {
    mem::write<uint32_t>(out, settings().width);
    mem::write<uint32_t>(out + 4, settings().height);
    mem::write<uint32_t>(out + 8, settings().refresh);
    mem::write<uint32_t>(out + 12, D::fmt::x8r8g8b8);
}

uint32_t dev_get_display_mode(uint32_t self, uint32_t chain, uint32_t out) {
    if (out == 0) {
        return D::kInvalidCall;
    }
    write_mode(out);
    return D::kOk;
}

uint32_t dev_get_creation_parameters(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, 0);
    mem::write<uint32_t>(out + 4, 1);
    mem::write<uint32_t>(out + 8, d->focus);
    mem::write<uint32_t>(out + 12, d->behavior);
    return D::kOk;
}

uint32_t dev_set_cursor_properties(uint32_t self, uint32_t x, uint32_t y, uint32_t bitmap) { return D::kOk; }
uint32_t dev_set_cursor_position(uint32_t self, uint32_t x, uint32_t y, uint32_t flags) { return D::kOk; }
uint32_t dev_show_cursor(uint32_t self, uint32_t show) { return 0; }

uint32_t dev_get_swap_chain(uint32_t self, uint32_t index, uint32_t out) {
    Device* d = dev(self);
    if (!d || index != 0 || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->swapchain);
    com_out(out, d->swapchain);
    return D::kOk;
}

uint32_t dev_get_number_of_swap_chains(uint32_t self) { return 1; }

uint32_t make_target_surface(Device& d, uint32_t self, uint32_t width, uint32_t height, uint32_t format, bool depth) {
    auto s = std::make_unique<Surface>();
    s->device = &d;
    s->device_guest = self;
    s->width = width;
    s->height = height;
    s->format = format;
    s->usage = depth ? D::usage::depth_stencil : D::usage::render_target;
    s->pool = D::pool::default_;
    TextureInfo info;
    info.width = width;
    info.height = height;
    info.format = depth ? GpuFormat::depth : GpuFormat::bgra8;
    info.render_target = true;
    info.opaque = format == D::fmt::x8r8g8b8 || format == D::fmt::r5g6b5 || format == D::fmt::x1r5g5b5;
    s->gpu = d.renderer->create_texture(info);
    s->owns_gpu = true;
    return com_objects().wrap(g_surface_iface, std::move(s));
}

void full_viewport(Device& d) {
    auto* rt = com_get<Surface>(d.render_target);
    if (!rt) {
        return;
    }
    d.st.viewport = Viewport{0, 0, rt->width, rt->height, 0, 1};
    d.st.scissor = Rect{0, 0, static_cast<int32_t>(rt->width), static_cast<int32_t>(rt->height)};
}

void setup_swap_chain(Device& d, uint32_t self) {
    auto& pp = d.present;
    uint32_t width = pp[0];
    uint32_t height = pp[1];
    if (width == 0 || height == 0) {
        for (const auto& w : windows_snapshot()) {
            if (w.hwnd == d.window) {
                width = static_cast<uint32_t>(std::max(w.width, 1));
                height = static_cast<uint32_t>(std::max(w.height, 1));
            }
        }
    }
    if (width == 0 || height == 0) {
        width = settings().width;
        height = settings().height;
    }
    pp[0] = width;
    pp[1] = height;
    if (pp[2] == D::fmt::unknown) {
        pp[2] = D::fmt::x8r8g8b8;
    }
    if (pp[3] == 0) {
        pp[3] = 1;
    }
    rebind(d.render_target, 0);
    rebind(d.depth_stencil, 0);
    if (d.backbuffer) {
        com_objects().destroy(d.backbuffer);
    }
    if (d.auto_depth) {
        com_objects().destroy(d.auto_depth);
        d.auto_depth = 0;
    }
    d.backbuffer = make_target_surface(d, self, width, height, pp[2], false);
    rebind(d.render_target, d.backbuffer);
    if (pp[9]) {
        d.auto_depth = make_target_surface(d, self, width, height, pp[10] ? pp[10] : D::fmt::d24s8, true);
        rebind(d.depth_stencil, d.auto_depth);
    }
    reset_state(d.st);
    d.st.rs[D::rs::zenable] = pp[9] ? 1 : 0;
    full_viewport(d);
    set_client_size(d.window, width, height);
    d.renderer->set_layer(platform::metal_layer(d.window));
    std::fprintf(stderr, "[D3D9] swap chain %ux%u format=%u windowed=%u depth=%u/%u interval=0x%x\n", width, height,
                 pp[2], pp[8], pp[9], pp[10], pp[13]);
}

uint32_t dev_reset(uint32_t self, uint32_t params) {
    Device* d = dev(self);
    if (!d || params == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(d->present.data(), mem::ptr(params), sizeof(d->present));
    setup_swap_chain(*d, self);
    std::memcpy(mem::ptr(params), d->present.data(), sizeof(d->present));
    return D::kOk;
}

uint32_t dev_present(uint32_t self, uint32_t src, uint32_t dst, uint32_t hwnd, uint32_t dirty) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    auto* bb = com_get<Surface>(d->backbuffer);
    d->renderer->present(bb ? bb->gpu : nullptr);
    uint64_t frame = d->renderer->frame();
    if (frame == 1 || frame == 10 || frame % 600 == 0) {
        std::fprintf(stderr, "[D3D9] frame %llu: %llu draws so far, %llu shader draws skipped\n",
                     static_cast<unsigned long long>(frame), static_cast<unsigned long long>(d->draws),
                     static_cast<unsigned long long>(d->skipped_shader_draws));
    }
    input::run_key_script(false, frame);
    platform::pump_events();
    if (settings().exit_after_frames != 0 && frame >= settings().exit_after_frames) {
        std::fprintf(stderr, "[D3D9] exiting after %llu frames as requested\n", static_cast<unsigned long long>(frame));
        std::fflush(stderr);
        d->renderer.reset();
        std::_Exit(0);
    }
    return D::kOk;
}

uint32_t dev_get_back_buffer(uint32_t self, uint32_t chain, uint32_t index, uint32_t type, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0 || index != 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->backbuffer);
    com_out(out, d->backbuffer);
    return D::kOk;
}

uint32_t dev_get_raster_status(uint32_t self, uint32_t chain, uint32_t out) { return sc_get_raster_status(0, out); }
uint32_t dev_set_dialog_box_mode(uint32_t self, uint32_t enable) { return D::kOk; }
uint32_t dev_set_gamma_ramp(uint32_t self, uint32_t chain, uint32_t flags, uint32_t ramp) { return 0; }

uint32_t dev_get_gamma_ramp(uint32_t self, uint32_t chain, uint32_t ramp) {
    if (ramp != 0) {
        for (uint32_t c = 0; c < 3; ++c) {
            for (uint32_t i = 0; i < 256; ++i) {
                mem::write<uint16_t>(ramp + (c * 256 + i) * 2, static_cast<uint16_t>(i * 257));
            }
        }
    }
    return 0;
}

uint32_t create_texture(Device& d, uint32_t self, uint32_t width, uint32_t height, uint32_t levels, uint32_t usage,
                        uint32_t format, uint32_t pool, uint32_t out, bool cube) {
    if (out == 0 || width == 0 || height == 0) {
        return D::kInvalidCall;
    }
    com_out(out, 0);
    FormatInfo fi = format_info(format);
    bool rt = (usage & D::usage::render_target) != 0;
    if (!fi.texture && !(rt && fi.render_target) && !fi.depth) {
        log_once("CreateTexture: unsupported format " + std::to_string(format));
        return D::kInvalidCall;
    }
    auto t = std::make_unique<Texture>();
    t->device = &d;
    t->device_guest = self;
    t->width = width;
    t->height = height;
    uint32_t full = full_mip_count(width, height);
    t->levels = (usage & D::usage::autogen_mipmap) ? 1 : (levels == 0 ? full : std::min(levels, full));
    t->gpu_levels = (usage & D::usage::autogen_mipmap) ? full : t->levels;
    t->usage = usage;
    t->format = format;
    t->pool = pool;
    t->faces = cube ? 6 : 1;
    t->mips.resize(t->faces * t->levels);
    t->surfaces.resize(t->faces * t->levels);
    if (pool != D::pool::systemmem && pool != D::pool::scratch) {
        TextureInfo info;
        info.width = width;
        info.height = height;
        info.levels = t->gpu_levels;
        info.format = fi.depth ? GpuFormat::depth : fi.gpu;
        info.cube = cube;
        info.render_target = rt;
        info.opaque = format == D::fmt::x8r8g8b8 || format == D::fmt::r5g6b5 || format == D::fmt::x1r5g5b5;
        t->gpu = d.renderer->create_texture(info);
        if (!t->gpu) {
            return D::kOutOfVideoMemory;
        }
    }
    com_out(out, com_objects().wrap(cube ? g_cube_iface : g_texture_iface, std::move(t)));
    return D::kOk;
}

uint32_t dev_create_texture(uint32_t self, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t format,
                            uint32_t pool, uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    return d ? create_texture(*d, self, w, h, levels, usage, format, pool, out, false) : D::kInvalidCall;
}

uint32_t dev_create_cube_texture(uint32_t self, uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format,
                                 uint32_t pool, uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    return d ? create_texture(*d, self, edge, edge, levels, usage, format, pool, out, true) : D::kInvalidCall;
}

uint32_t dev_create_volume_texture(uint32_t self, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                                   uint32_t usage, uint32_t format, uint32_t pool, uint32_t out, uint32_t shared) {
    log_once("CreateVolumeTexture refused");
    com_out(out, 0);
    return D::kNotAvailable;
}

uint32_t create_buffer(Device& d, uint32_t self, uint32_t length, uint32_t usage, uint32_t format, uint32_t pool,
                       uint32_t out, bool index) {
    if (out == 0 || length == 0) {
        return D::kInvalidCall;
    }
    auto b = std::make_unique<Buffer>();
    b->device = &d;
    b->device_guest = self;
    b->size = length;
    b->usage = usage;
    b->format = index ? (format == D::fmt::index32 ? D::fmt::index32 : D::fmt::index16) : format;
    b->pool = pool;
    b->index = index;
    b->storage = allocator().alloc(length);
    if (b->storage == 0) {
        com_out(out, 0);
        return D::kOutOfVideoMemory;
    }
    com_out(out, com_objects().wrap(index ? g_ib_iface : g_vb_iface, std::move(b)));
    return D::kOk;
}

uint32_t dev_create_vertex_buffer(uint32_t self, uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool,
                                  uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    return d ? create_buffer(*d, self, length, usage, fvf, pool, out, false) : D::kInvalidCall;
}

uint32_t dev_create_index_buffer(uint32_t self, uint32_t length, uint32_t usage, uint32_t format, uint32_t pool,
                                 uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    return d ? create_buffer(*d, self, length, usage, format, pool, out, true) : D::kInvalidCall;
}

uint32_t dev_create_render_target(uint32_t self, uint32_t w, uint32_t h, uint32_t format, uint32_t ms, uint32_t q,
                                  uint32_t lockable, uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    com_out(out, make_target_surface(*d, self, w, h, format, false));
    return D::kOk;
}

uint32_t dev_create_depth_stencil_surface(uint32_t self, uint32_t w, uint32_t h, uint32_t format, uint32_t ms,
                                          uint32_t q, uint32_t discard, uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    com_out(out, make_target_surface(*d, self, w, h, format, true));
    return D::kOk;
}

uint32_t dev_create_offscreen_plain_surface(uint32_t self, uint32_t w, uint32_t h, uint32_t format, uint32_t pool,
                                            uint32_t out, uint32_t shared) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    auto s = std::make_unique<Surface>();
    s->device = d;
    s->device_guest = self;
    s->width = w;
    s->height = h;
    s->format = format;
    s->pool = pool;
    s->pitch = level_pitch(format, w);
    s->storage = allocator().alloc(s->pitch * level_rows(format, h));
    com_out(out, com_objects().wrap(g_surface_iface, std::move(s)));
    return D::kOk;
}

uint32_t surface_storage(Surface& s, uint32_t& pitch) {
    if (s.texture) {
        Texture::Level& lv = s.texture->level(s.face, s.level);
        if (lv.storage == 0) {
            uint32_t tmp = allocator().alloc(8);
            lock_rect(*s.texture, s.face, s.level, tmp, 0, 0);
            allocator().free(tmp);
        }
        pitch = lv.pitch;
        return lv.storage;
    }
    if (s.storage == 0) {
        s.pitch = level_pitch(s.format, s.width);
        s.storage = allocator().alloc(s.pitch * level_rows(s.format, s.height));
    }
    pitch = s.pitch;
    return s.storage;
}

uint32_t dev_update_surface(uint32_t self, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_point) {
    auto* s = com_get<Surface>(src);
    auto* t = com_get<Surface>(dst);
    if (!s || !t || s->format != t->format) {
        return D::kInvalidCall;
    }
    if (src_rect != 0 || dst_point != 0) {
        log_once("UpdateSurface with sub-rectangles copies the whole surface");
    }
    uint32_t sp = 0;
    uint32_t dp = 0;
    uint32_t sa = surface_storage(*s, sp);
    uint32_t da = surface_storage(*t, dp);
    uint32_t rows = std::min(level_rows(s->format, s->height), level_rows(t->format, t->height));
    uint32_t row = std::min(sp, dp);
    for (uint32_t y = 0; y < rows; ++y) {
        std::memcpy(mem::ptr(da + y * dp), mem::ptr(sa + y * sp), row);
    }
    if (t->texture) {
        upload_level(*t->texture, t->face, t->level);
    }
    return D::kOk;
}

uint32_t dev_update_texture(uint32_t self, uint32_t src, uint32_t dst) {
    auto* s = com_get<Texture>(src);
    auto* t = com_get<Texture>(dst);
    if (!s || !t || s->format != t->format || s->faces != t->faces) {
        return D::kInvalidCall;
    }
    uint32_t levels = std::min(s->levels, t->levels);
    for (uint32_t f = 0; f < t->faces; ++f) {
        for (uint32_t l = 0; l < levels; ++l) {
            Texture::Level& a = s->level(f, l);
            if (a.storage == 0) {
                continue;
            }
            uint32_t tmp = allocator().alloc(8);
            lock_rect(*t, f, l, tmp, 0, 0);
            allocator().free(tmp);
            Texture::Level& b = t->level(f, l);
            std::memcpy(mem::ptr(b.storage), mem::ptr(a.storage), std::min(a.size, b.size));
            upload_level(*t, f, l);
        }
    }
    return D::kOk;
}

uint32_t dev_get_render_target_data(uint32_t self, uint32_t src, uint32_t dst) {
    Device* d = dev(self);
    auto* s = com_get<Surface>(src);
    auto* t = com_get<Surface>(dst);
    if (!d || !s || !t) {
        return D::kInvalidCall;
    }
    uint32_t pitch = 0;
    uint32_t storage = surface_storage(*t, pitch);
    return d->renderer->read_back(target_of(*s), mem::ptr(storage), pitch) ? D::kOk : D::kInvalidCall;
}

uint32_t dev_get_front_buffer_data(uint32_t self, uint32_t chain, uint32_t dst) {
    Device* d = dev(self);
    return d ? dev_get_render_target_data(self, d->backbuffer, dst) : D::kInvalidCall;
}

Rect read_rect_or(uint32_t addr, uint32_t w, uint32_t h) {
    if (addr == 0) {
        return Rect{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
    }
    return Rect{mem::read<int32_t>(addr), mem::read<int32_t>(addr + 4), mem::read<int32_t>(addr + 8),
                mem::read<int32_t>(addr + 12)};
}

uint32_t dev_stretch_rect(uint32_t self, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect,
                          uint32_t filter) {
    Device* d = dev(self);
    auto* s = com_get<Surface>(src);
    auto* t = com_get<Surface>(dst);
    if (!d || !s || !t) {
        return D::kInvalidCall;
    }
    d->renderer->stretch(target_of(*s), read_rect_or(src_rect, s->width, s->height), target_of(*t),
                         read_rect_or(dst_rect, t->width, t->height), filter >= 2);
    return D::kOk;
}

uint32_t dev_color_fill(uint32_t self, uint32_t surface, uint32_t rect, uint32_t color) {
    Device* d = dev(self);
    auto* s = com_get<Surface>(surface);
    if (!d || !s || !target_of(*s).texture) {
        return D::kInvalidCall;
    }
    auto* rt = com_get<Surface>(d->render_target);
    auto* ds = com_get<Surface>(d->depth_stencil);
    d->renderer->set_targets(target_of(*s), nullptr);
    Rect r = read_rect_or(rect, s->width, s->height);
    Viewport vp{0, 0, s->width, s->height, 0, 1};
    d->renderer->clear(D::clear::target, std::span<const Rect>(&r, 1), vp, color, 0, 0);
    d->renderer->set_targets(rt ? target_of(*rt) : RenderTarget{}, ds ? ds->gpu : nullptr);
    return D::kOk;
}

uint32_t dev_set_render_target(uint32_t self, uint32_t index, uint32_t surface) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (index != 0) {
        if (surface != 0) {
            log_once("SetRenderTarget with index > 0 ignored");
        }
        return D::kOk;
    }
    auto* s = com_get<Surface>(surface);
    if (!s) {
        return D::kInvalidCall;
    }
    rebind(d->render_target, surface);
    full_viewport(*d);
    return D::kOk;
}

uint32_t dev_get_render_target(uint32_t self, uint32_t index, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t s = index == 0 ? d->render_target : 0;
    com_out(out, s);
    if (s == 0) {
        return D::kNotFound;
    }
    surface_add_ref(s);
    return D::kOk;
}

uint32_t dev_set_depth_stencil_surface(uint32_t self, uint32_t surface) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    rebind(d->depth_stencil, surface);
    return D::kOk;
}

uint32_t dev_get_depth_stencil_surface(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    com_out(out, d->depth_stencil);
    if (d->depth_stencil == 0) {
        return D::kNotFound;
    }
    surface_add_ref(d->depth_stencil);
    return D::kOk;
}

uint32_t dev_begin_scene(uint32_t self) { return D::kOk; }
uint32_t dev_end_scene(uint32_t self) { return D::kOk; }

uint32_t dev_clear(uint32_t self, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    auto* rt = com_get<Surface>(d->render_target);
    auto* ds = com_get<Surface>(d->depth_stencil);
    d->renderer->set_targets(rt ? target_of(*rt) : RenderTarget{}, ds ? ds->gpu : nullptr);
    std::vector<Rect> list;
    if (rects != 0) {
        for (uint32_t i = 0; i < count; ++i) {
            list.push_back(read_rect_or(rects + 16 * i, 0, 0));
        }
    }
    d->renderer->clear(flags, list, d->st.viewport, color, z, stencil);
    return D::kOk;
}

Matrix* transform_slot(Device& d, uint32_t state) {
    if (state == D::ts::view) {
        return &d.st.view;
    }
    if (state == D::ts::projection) {
        return &d.st.projection;
    }
    if (state >= D::ts::texture0 && state < D::ts::texture0 + kMaxStages) {
        return &d.st.texture_transform[state - D::ts::texture0];
    }
    if (state == D::ts::world) {
        return &d.st.world;
    }
    return nullptr;
}

uint32_t dev_set_transform(uint32_t self, uint32_t state, uint32_t m) {
    Device* d = dev(self);
    if (!d || m == 0) {
        return D::kInvalidCall;
    }
    if (Matrix* slot = transform_slot(*d, state)) {
        std::memcpy(slot->m, mem::ptr(m), 64);
    }
    return D::kOk;
}

uint32_t dev_get_transform(uint32_t self, uint32_t state, uint32_t m) {
    Device* d = dev(self);
    if (!d || m == 0) {
        return D::kInvalidCall;
    }
    Matrix identity;
    Matrix* slot = transform_slot(*d, state);
    std::memcpy(mem::ptr(m), slot ? slot->m : identity.m, 64);
    return D::kOk;
}

uint32_t dev_multiply_transform(uint32_t self, uint32_t state, uint32_t m) {
    Device* d = dev(self);
    if (!d || m == 0) {
        return D::kInvalidCall;
    }
    if (Matrix* slot = transform_slot(*d, state)) {
        Matrix in;
        std::memcpy(in.m, mem::ptr(m), 64);
        *slot = multiply(in, *slot);
    }
    return D::kOk;
}

uint32_t dev_set_viewport(uint32_t self, uint32_t vp) {
    Device* d = dev(self);
    if (!d || vp == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(&d->st.viewport, mem::ptr(vp), sizeof(Viewport));
    return D::kOk;
}

uint32_t dev_get_viewport(uint32_t self, uint32_t vp) {
    Device* d = dev(self);
    if (!d || vp == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(vp), &d->st.viewport, sizeof(Viewport));
    return D::kOk;
}

uint32_t dev_set_material(uint32_t self, uint32_t m) {
    Device* d = dev(self);
    if (!d || m == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(&d->st.material, mem::ptr(m), sizeof(Material));
    return D::kOk;
}

uint32_t dev_get_material(uint32_t self, uint32_t m) {
    Device* d = dev(self);
    if (!d || m == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(m), &d->st.material, sizeof(Material));
    return D::kOk;
}

uint32_t dev_set_light(uint32_t self, uint32_t index, uint32_t l) {
    Device* d = dev(self);
    if (!d || l == 0) {
        return D::kInvalidCall;
    }
    if (index >= kMaxLights) {
        log_once("SetLight index >= 8 ignored");
        return D::kOk;
    }
    std::memcpy(&d->st.lights[index], mem::ptr(l), sizeof(Light));
    return D::kOk;
}

uint32_t dev_get_light(uint32_t self, uint32_t index, uint32_t l) {
    Device* d = dev(self);
    if (!d || l == 0 || index >= kMaxLights) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(l), &d->st.lights[index], sizeof(Light));
    return D::kOk;
}

uint32_t dev_light_enable(uint32_t self, uint32_t index, uint32_t enable) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (index < kMaxLights) {
        d->st.light_enabled[index] = enable ? 1 : 0;
    } else if (enable) {
        log_once("LightEnable index >= 8 ignored");
    }
    return D::kOk;
}

uint32_t dev_get_light_enable(uint32_t self, uint32_t index, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0 || index >= kMaxLights) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, d->st.light_enabled[index]);
    return D::kOk;
}

uint32_t dev_set_clip_plane(uint32_t self, uint32_t index, uint32_t plane) {
    Device* d = dev(self);
    if (!d || plane == 0 || index >= 6) {
        return D::kInvalidCall;
    }
    std::memcpy(&d->clip_planes[index * 4], mem::ptr(plane), 16);
    return D::kOk;
}

uint32_t dev_get_clip_plane(uint32_t self, uint32_t index, uint32_t plane) {
    Device* d = dev(self);
    if (!d || plane == 0 || index >= 6) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(plane), &d->clip_planes[index * 4], 16);
    return D::kOk;
}

uint32_t dev_set_render_state(uint32_t self, uint32_t state, uint32_t value) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (state < D::rs::count) {
        d->st.rs[state] = value;
    }
    if (state == D::rs::clipplaneenable && value != 0) {
        log_once("user clip planes enabled; not implemented");
    }
    return D::kOk;
}

uint32_t dev_get_render_state(uint32_t self, uint32_t state, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, state < D::rs::count ? d->st.rs[state] : 0);
    return D::kOk;
}

uint32_t dev_get_texture(uint32_t self, uint32_t stage, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t t = stage < kMaxSamplers ? d->textures[stage] : 0;
    add_ref_guest(t);
    com_out(out, t);
    return D::kOk;
}

uint32_t dev_set_texture(uint32_t self, uint32_t stage, uint32_t texture) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (stage < kMaxSamplers) {
        rebind(d->textures[stage], texture);
    }
    return D::kOk;
}

uint32_t dev_get_texture_stage_state(uint32_t self, uint32_t stage, uint32_t type, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0 || stage >= kMaxStages || type >= D::tss::count) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, d->st.tss[stage][type]);
    return D::kOk;
}

uint32_t dev_set_texture_stage_state(uint32_t self, uint32_t stage, uint32_t type, uint32_t value) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (stage < kMaxStages && type < D::tss::count) {
        d->st.tss[stage][type] = value;
    }
    return D::kOk;
}

uint32_t dev_get_sampler_state(uint32_t self, uint32_t sampler, uint32_t type, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0 || sampler >= kMaxSamplers || type >= D::samp::count) {
        return D::kInvalidCall;
    }
    mem::write<uint32_t>(out, d->st.ss[sampler][type]);
    return D::kOk;
}

uint32_t dev_set_sampler_state(uint32_t self, uint32_t sampler, uint32_t type, uint32_t value) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    if (sampler < kMaxSamplers && type < D::samp::count) {
        d->st.ss[sampler][type] = value;
    }
    return D::kOk;
}

uint32_t dev_validate_device(uint32_t self, uint32_t out) {
    com_out(out, 1);
    return D::kOk;
}

uint32_t dev_set_palette_entries(uint32_t self, uint32_t index, uint32_t entries) { return D::kOk; }

uint32_t dev_set_current_texture_palette(uint32_t self, uint32_t index) {
    if (Device* d = dev(self)) {
        d->palette = index;
    }
    return D::kOk;
}

uint32_t dev_get_current_texture_palette(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    com_out(out, d ? d->palette : 0);
    return D::kOk;
}

uint32_t dev_set_scissor_rect(uint32_t self, uint32_t rect) {
    Device* d = dev(self);
    if (!d || rect == 0) {
        return D::kInvalidCall;
    }
    d->st.scissor = read_rect_or(rect, 0, 0);
    return D::kOk;
}

uint32_t dev_get_scissor_rect(uint32_t self, uint32_t rect) {
    Device* d = dev(self);
    if (!d || rect == 0) {
        return D::kInvalidCall;
    }
    std::memcpy(mem::ptr(rect), &d->st.scissor, 16);
    return D::kOk;
}

uint32_t dev_set_software_vp(uint32_t self, uint32_t software) {
    if (Device* d = dev(self)) {
        d->software_vp = software != 0;
    }
    return D::kOk;
}

uint32_t dev_get_software_vp(uint32_t self) {
    Device* d = dev(self);
    return d && d->software_vp ? 1 : 0;
}

uint32_t dev_set_npatch_mode(uint32_t self, float segments) {
    if (Device* d = dev(self)) {
        d->npatch = segments;
    }
    return D::kOk;
}

float dev_get_npatch_mode(uint32_t self) {
    Device* d = dev(self);
    return d ? d->npatch : 0.0f;
}

uint32_t dev_create_vertex_declaration(uint32_t self, uint32_t elements, uint32_t out) {
    Device* d = dev(self);
    if (!d || elements == 0 || out == 0) {
        return D::kInvalidCall;
    }
    auto decl = std::make_unique<VertexDecl>();
    decl->device_guest = self;
    for (uint32_t i = 0; i < 65; ++i) {
        const uint8_t* e = mem::ptr(elements + 8 * i);
        decl->elements.insert(decl->elements.end(), e, e + 8);
        if (mem::read<uint16_t>(elements + 8 * i) == 0xff) {
            break;
        }
    }
    decl->layout = layout_from_declaration(decl->elements);
    com_out(out, com_objects().wrap(g_decl_iface, std::move(decl)));
    return D::kOk;
}

uint32_t dev_set_vertex_declaration(uint32_t self, uint32_t decl) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    rebind(d->decl, decl);
    d->use_decl = decl != 0;
    return D::kOk;
}

uint32_t dev_get_vertex_declaration(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    uint32_t decl = d->use_decl ? d->decl : 0;
    add_ref_guest(decl);
    com_out(out, decl);
    return D::kOk;
}

uint32_t dev_set_fvf(uint32_t self, uint32_t fvf) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    d->fvf = fvf;
    if (fvf != 0) {
        d->use_decl = false;
    }
    return D::kOk;
}

uint32_t dev_get_fvf(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    com_out(out, d && !d->use_decl ? d->fvf : 0);
    return D::kOk;
}

uint32_t create_shader(uint32_t self, uint32_t function, uint32_t out, bool pixel) {
    Device* d = dev(self);
    if (!d || function == 0 || out == 0) {
        return D::kInvalidCall;
    }
    auto s = std::make_unique<Shader>();
    s->device_guest = self;
    s->pixel = pixel;
    uint32_t version = mem::read<uint32_t>(function);
    uint32_t n = 1;
    while (n < 65536 && mem::read<uint32_t>(function + 4 * n) != 0x0000ffffu) {
        ++n;
    }
    ++n;
    s->code.resize(n * 4);
    std::memcpy(s->code.data(), mem::ptr(function), n * 4);
    log_once(std::string(pixel ? "pixel" : "vertex") + " shaders created (first version token " +
             std::to_string(version) + ")");
    com_out(out, com_objects().wrap(pixel ? g_pshader_iface : g_vshader_iface, std::move(s)));
    return D::kOk;
}

uint32_t dev_create_vertex_shader(uint32_t self, uint32_t function, uint32_t out) {
    return create_shader(self, function, out, false);
}

uint32_t dev_create_pixel_shader(uint32_t self, uint32_t function, uint32_t out) {
    return create_shader(self, function, out, true);
}

uint32_t dev_set_vertex_shader(uint32_t self, uint32_t shader) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    rebind(d->vertex_shader, shader);
    return D::kOk;
}

uint32_t dev_get_vertex_shader(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->vertex_shader);
    com_out(out, d->vertex_shader);
    return D::kOk;
}

uint32_t dev_set_pixel_shader(uint32_t self, uint32_t shader) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    rebind(d->pixel_shader, shader);
    return D::kOk;
}

uint32_t dev_get_pixel_shader(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->pixel_shader);
    com_out(out, d->pixel_shader);
    return D::kOk;
}

template <typename T, size_t N>
uint32_t copy_constants(T* store, size_t store_count, uint32_t start, uint32_t data, uint32_t count, bool set) {
    if (data == 0 || static_cast<size_t>(start) + count > store_count) {
        return D::kInvalidCall;
    }
    size_t bytes = static_cast<size_t>(count) * N * sizeof(T);
    if (set) {
        std::memcpy(store + start * N, mem::ptr(data), bytes);
    } else {
        std::memcpy(mem::ptr(data), store + start * N, bytes);
    }
    return D::kOk;
}

uint32_t dev_set_vs_const_f(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<float, 4>(d->vs_const.data(), 256, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_vs_const_f(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<float, 4>(d->vs_const.data(), 256, start, data, count, false) : D::kInvalidCall;
}
uint32_t dev_set_vs_const_i(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<int32_t, 4>(d->vs_int.data(), 16, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_vs_const_i(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<int32_t, 4>(d->vs_int.data(), 16, start, data, count, false) : D::kInvalidCall;
}
uint32_t dev_set_vs_const_b(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<uint32_t, 1>(d->vs_bool.data(), 16, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_vs_const_b(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<uint32_t, 1>(d->vs_bool.data(), 16, start, data, count, false) : D::kInvalidCall;
}
uint32_t dev_set_ps_const_f(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<float, 4>(d->ps_const.data(), 224, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_ps_const_f(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<float, 4>(d->ps_const.data(), 224, start, data, count, false) : D::kInvalidCall;
}
uint32_t dev_set_ps_const_i(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<int32_t, 4>(d->ps_int.data(), 16, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_ps_const_i(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<int32_t, 4>(d->ps_int.data(), 16, start, data, count, false) : D::kInvalidCall;
}
uint32_t dev_set_ps_const_b(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<uint32_t, 1>(d->ps_bool.data(), 16, start, data, count, true) : D::kInvalidCall;
}
uint32_t dev_get_ps_const_b(uint32_t self, uint32_t start, uint32_t data, uint32_t count) {
    Device* d = dev(self);
    return d ? copy_constants<uint32_t, 1>(d->ps_bool.data(), 16, start, data, count, false) : D::kInvalidCall;
}

uint32_t dev_set_stream_source(uint32_t self, uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride) {
    Device* d = dev(self);
    if (!d || stream >= kMaxStreams) {
        return D::kInvalidCall;
    }
    rebind(d->streams[stream].buffer, buffer);
    d->streams[stream].offset = offset;
    d->streams[stream].stride = stride;
    return D::kOk;
}

uint32_t dev_get_stream_source(uint32_t self, uint32_t stream, uint32_t out, uint32_t offset, uint32_t stride) {
    Device* d = dev(self);
    if (!d || stream >= kMaxStreams || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->streams[stream].buffer);
    com_out(out, d->streams[stream].buffer);
    com_out(offset, d->streams[stream].offset);
    com_out(stride, d->streams[stream].stride);
    return D::kOk;
}

uint32_t dev_set_stream_source_freq(uint32_t self, uint32_t stream, uint32_t setting) {
    Device* d = dev(self);
    if (!d || stream >= kMaxStreams) {
        return D::kInvalidCall;
    }
    d->streams[stream].freq = setting;
    if (setting != 1) {
        log_once("instanced stream frequency set; not implemented");
    }
    return D::kOk;
}

uint32_t dev_get_stream_source_freq(uint32_t self, uint32_t stream, uint32_t out) {
    Device* d = dev(self);
    if (!d || stream >= kMaxStreams) {
        return D::kInvalidCall;
    }
    com_out(out, d->streams[stream].freq);
    return D::kOk;
}

uint32_t dev_set_indices(uint32_t self, uint32_t buffer) {
    Device* d = dev(self);
    if (!d) {
        return D::kInvalidCall;
    }
    rebind(d->indices, buffer);
    return D::kOk;
}

uint32_t dev_get_indices(uint32_t self, uint32_t out) {
    Device* d = dev(self);
    if (!d || out == 0) {
        return D::kInvalidCall;
    }
    add_ref_guest(d->indices);
    com_out(out, d->indices);
    return D::kOk;
}

uint32_t dev_create_query(uint32_t self, uint32_t type, uint32_t out) {
    if (type != 8 && type != 9) {
        log_once("CreateQuery type " + std::to_string(type) + " refused");
        com_out(out, 0);
        return D::kNotAvailable;
    }
    if (out == 0) {
        return D::kOk;
    }
    auto q = std::make_unique<Query>();
    q->type = type;
    q->device_guest = self;
    com_out(out, com_objects().wrap(g_query_iface, std::move(q)));
    return D::kOk;
}

uint32_t dev_release(uint32_t self) { return com_obj_release(self); }

ComInterface g_device_iface(
    "d3d9.dll", "IDirect3DDevice9",
    {
        com_method<query_interface>("QueryInterface"),
        com_method<com_obj_add_ref>("AddRef"),
        com_method<dev_release>("Release"),
        com_method<dev_test_cooperative_level>("TestCooperativeLevel"),
        com_method<dev_get_available_texture_mem>("GetAvailableTextureMem"),
        com_method<dev_evict_managed>("EvictManagedResources"),
        com_method<dev_get_direct3d>("GetDirect3D"),
        com_method<dev_get_device_caps>("GetDeviceCaps"),
        com_method<dev_get_display_mode>("GetDisplayMode"),
        com_method<dev_get_creation_parameters>("GetCreationParameters"),
        com_method<dev_set_cursor_properties>("SetCursorProperties"),
        com_method<dev_set_cursor_position>("SetCursorPosition"),
        com_method<dev_show_cursor>("ShowCursor"),
        ComMethod{"CreateAdditionalSwapChain", 3, nullptr, D::kNotAvailable},
        com_method<dev_get_swap_chain>("GetSwapChain"),
        com_method<dev_get_number_of_swap_chains>("GetNumberOfSwapChains"),
        com_method<dev_reset>("Reset"),
        com_method<dev_present>("Present"),
        com_method<dev_get_back_buffer>("GetBackBuffer"),
        com_method<dev_get_raster_status>("GetRasterStatus"),
        com_method<dev_set_dialog_box_mode>("SetDialogBoxMode"),
        com_method<dev_set_gamma_ramp>("SetGammaRamp"),
        com_method<dev_get_gamma_ramp>("GetGammaRamp"),
        com_method<dev_create_texture>("CreateTexture"),
        com_method<dev_create_volume_texture>("CreateVolumeTexture"),
        com_method<dev_create_cube_texture>("CreateCubeTexture"),
        com_method<dev_create_vertex_buffer>("CreateVertexBuffer"),
        com_method<dev_create_index_buffer>("CreateIndexBuffer"),
        com_method<dev_create_render_target>("CreateRenderTarget"),
        com_method<dev_create_depth_stencil_surface>("CreateDepthStencilSurface"),
        com_method<dev_update_surface>("UpdateSurface"),
        com_method<dev_update_texture>("UpdateTexture"),
        com_method<dev_get_render_target_data>("GetRenderTargetData"),
        com_method<dev_get_front_buffer_data>("GetFrontBufferData"),
        com_method<dev_stretch_rect>("StretchRect"),
        com_method<dev_color_fill>("ColorFill"),
        com_method<dev_create_offscreen_plain_surface>("CreateOffscreenPlainSurface"),
        com_method<dev_set_render_target>("SetRenderTarget"),
        com_method<dev_get_render_target>("GetRenderTarget"),
        com_method<dev_set_depth_stencil_surface>("SetDepthStencilSurface"),
        com_method<dev_get_depth_stencil_surface>("GetDepthStencilSurface"),
        com_method<dev_begin_scene>("BeginScene"),
        com_method<dev_end_scene>("EndScene"),
        com_method<dev_clear>("Clear"),
        com_method<dev_set_transform>("SetTransform"),
        com_method<dev_get_transform>("GetTransform"),
        com_method<dev_multiply_transform>("MultiplyTransform"),
        com_method<dev_set_viewport>("SetViewport"),
        com_method<dev_get_viewport>("GetViewport"),
        com_method<dev_set_material>("SetMaterial"),
        com_method<dev_get_material>("GetMaterial"),
        com_method<dev_set_light>("SetLight"),
        com_method<dev_get_light>("GetLight"),
        com_method<dev_light_enable>("LightEnable"),
        com_method<dev_get_light_enable>("GetLightEnable"),
        com_method<dev_set_clip_plane>("SetClipPlane"),
        com_method<dev_get_clip_plane>("GetClipPlane"),
        com_method<dev_set_render_state>("SetRenderState"),
        com_method<dev_get_render_state>("GetRenderState"),
        ComMethod{"CreateStateBlock", 3, nullptr, D::kInvalidCall},
        ComMethod{"BeginStateBlock", 1, nullptr, D::kInvalidCall},
        ComMethod{"EndStateBlock", 2, nullptr, D::kInvalidCall},
        ComMethod{"SetClipStatus", 2},
        ComMethod{"GetClipStatus", 2},
        com_method<dev_get_texture>("GetTexture"),
        com_method<dev_set_texture>("SetTexture"),
        com_method<dev_get_texture_stage_state>("GetTextureStageState"),
        com_method<dev_set_texture_stage_state>("SetTextureStageState"),
        com_method<dev_get_sampler_state>("GetSamplerState"),
        com_method<dev_set_sampler_state>("SetSamplerState"),
        com_method<dev_validate_device>("ValidateDevice"),
        com_method<dev_set_palette_entries>("SetPaletteEntries"),
        ComMethod{"GetPaletteEntries", 3, nullptr, D::kInvalidCall},
        com_method<dev_set_current_texture_palette>("SetCurrentTexturePalette"),
        com_method<dev_get_current_texture_palette>("GetCurrentTexturePalette"),
        com_method<dev_set_scissor_rect>("SetScissorRect"),
        com_method<dev_get_scissor_rect>("GetScissorRect"),
        com_method<dev_set_software_vp>("SetSoftwareVertexProcessing"),
        com_method<dev_get_software_vp>("GetSoftwareVertexProcessing"),
        com_method<dev_set_npatch_mode>("SetNPatchMode"),
        com_method<dev_get_npatch_mode>("GetNPatchMode"),
        com_method<dev_draw_primitive>("DrawPrimitive"),
        com_method<dev_draw_indexed_primitive>("DrawIndexedPrimitive"),
        com_method<dev_draw_primitive_up>("DrawPrimitiveUP"),
        com_method<dev_draw_indexed_primitive_up>("DrawIndexedPrimitiveUP"),
        ComMethod{"ProcessVertices", 7, nullptr, D::kInvalidCall},
        com_method<dev_create_vertex_declaration>("CreateVertexDeclaration"),
        com_method<dev_set_vertex_declaration>("SetVertexDeclaration"),
        com_method<dev_get_vertex_declaration>("GetVertexDeclaration"),
        com_method<dev_set_fvf>("SetFVF"),
        com_method<dev_get_fvf>("GetFVF"),
        com_method<dev_create_vertex_shader>("CreateVertexShader"),
        com_method<dev_set_vertex_shader>("SetVertexShader"),
        com_method<dev_get_vertex_shader>("GetVertexShader"),
        com_method<dev_set_vs_const_f>("SetVertexShaderConstantF"),
        com_method<dev_get_vs_const_f>("GetVertexShaderConstantF"),
        com_method<dev_set_vs_const_i>("SetVertexShaderConstantI"),
        com_method<dev_get_vs_const_i>("GetVertexShaderConstantI"),
        com_method<dev_set_vs_const_b>("SetVertexShaderConstantB"),
        com_method<dev_get_vs_const_b>("GetVertexShaderConstantB"),
        com_method<dev_set_stream_source>("SetStreamSource"),
        com_method<dev_get_stream_source>("GetStreamSource"),
        com_method<dev_set_stream_source_freq>("SetStreamSourceFreq"),
        com_method<dev_get_stream_source_freq>("GetStreamSourceFreq"),
        com_method<dev_set_indices>("SetIndices"),
        com_method<dev_get_indices>("GetIndices"),
        com_method<dev_create_pixel_shader>("CreatePixelShader"),
        com_method<dev_set_pixel_shader>("SetPixelShader"),
        com_method<dev_get_pixel_shader>("GetPixelShader"),
        com_method<dev_set_ps_const_f>("SetPixelShaderConstantF"),
        com_method<dev_get_ps_const_f>("GetPixelShaderConstantF"),
        com_method<dev_set_ps_const_i>("SetPixelShaderConstantI"),
        com_method<dev_get_ps_const_i>("GetPixelShaderConstantI"),
        com_method<dev_set_ps_const_b>("SetPixelShaderConstantB"),
        com_method<dev_get_ps_const_b>("GetPixelShaderConstantB"),
        ComMethod{"DrawRectPatch", 4, nullptr, D::kInvalidCall},
        ComMethod{"DrawTriPatch", 4, nullptr, D::kInvalidCall},
        ComMethod{"DeletePatch", 2, nullptr, D::kInvalidCall},
        com_method<dev_create_query>("CreateQuery"),
    });

void fill_caps(uint32_t p) {
    std::memset(mem::ptr(p), 0, 304);
    auto w = [&](uint32_t off, uint32_t v) { mem::write<uint32_t>(p + off, v); };
    auto f = [&](uint32_t off, float v) { mem::write<float>(p + off, v); };
    w(0, 1);
    w(4, 0);
    w(8, 0x20000);
    w(12, 0x40000000u | 0x20000000u | 0x20000u);
    w(16, 0x20 | 0x100 | 0x200);
    w(20, 0x80000000u | 1);
    w(24, 1);
    w(28, 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 |
              0x80000 | 0x100000);
    w(32, 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000);
    w(36, 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | 0x400000 | 0x1000000 |
              0x2000000 | 0x4000000);
    w(40, 0xff);
    w(44, 0x3fff);
    w(48, 0x3fff);
    w(52, 0xff);
    w(56, 0x8 | 0x200 | 0x4000 | 0x80000);
    w(60, 0x1 | 0x4 | 0x400 | 0x800 | 0x4000 | 0x10000);
    uint32_t filters = 0x100 | 0x200 | 0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000 | 0x4000000;
    w(64, filters);
    w(68, filters);
    w(76, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20);
    w(84, 0x1 | 0x2 | 0x4 | 0x8 | 0x10);
    w(88, 8192);
    w(92, 8192);
    w(100, 8192);
    w(104, 8192);
    w(108, 16);
    f(112, 1e10f);
    f(116, -1e8f);
    f(120, -1e8f);
    f(124, 1e8f);
    f(128, 1e8f);
    w(136, 0x1ff);
    w(140, 8 | 0x100000);
    w(144, 0x3ffffff);
    w(148, 8);
    w(152, 8);
    w(156, 0x1 | 0x2 | 0x8 | 0x10 | 0x20);
    w(160, 8);
    f(176, 64.0f);
    w(180, 0xfffff);
    w(184, 0xffffff);
    w(188, 8);
    w(192, 255);
    w(196, 0xfffe0200u);
    w(200, 256);
    w(204, 0xffff0200u);
    f(208, 65504.0f);
    w(212, 0x1 | 0x40);
    w(232, 1);
    w(236, 0x3ff);
    w(240, 1);
    w(244, 0x100 | 0x200 | 0x1000000 | 0x2000000);
    w(248, 1);
    w(252, 24);
    w(256, 32);
    w(260, 4);
    w(264, 0x1f);
    w(268, 24);
    w(272, 32);
    w(276, 4);
    w(280, 512);
    w(288, 65535);
    w(292, 65535);
}

uint32_t d3d_get_adapter_count(uint32_t self) { return 1; }

uint32_t d3d_get_adapter_identifier(uint32_t self, uint32_t adapter, uint32_t flags, uint32_t out) {
    if (adapter != 0 || out == 0) {
        return D::kInvalidCall;
    }
    std::memset(mem::ptr(out), 0, 1100);
    mem::write_cstr(out, "macandreas.dll");
    mem::write_cstr(out + 512, "MacAndreas Metal");
    mem::write_cstr(out + 1024, "\\\\.\\DISPLAY1");
    mem::write<uint32_t>(out + 1056, 0x00010000);
    mem::write<uint32_t>(out + 1060, 0x00060000);
    mem::write<uint32_t>(out + 1064, 0x106b);
    mem::write<uint32_t>(out + 1068, 0x0001);
    mem::write<uint32_t>(out + 1096, 1);
    return D::kOk;
}

uint32_t d3d_get_adapter_mode_count(uint32_t self, uint32_t adapter, uint32_t format) {
    return adapter == 0 && format == D::fmt::x8r8g8b8 ? 1 : 0;
}

uint32_t d3d_enum_adapter_modes(uint32_t self, uint32_t adapter, uint32_t format, uint32_t mode, uint32_t out) {
    if (adapter != 0 || format != D::fmt::x8r8g8b8 || mode != 0 || out == 0) {
        return D::kInvalidCall;
    }
    write_mode(out);
    return D::kOk;
}

uint32_t d3d_get_adapter_display_mode(uint32_t self, uint32_t adapter, uint32_t out) {
    if (adapter != 0 || out == 0) {
        return D::kInvalidCall;
    }
    write_mode(out);
    return D::kOk;
}

uint32_t d3d_check_device_type(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format,
                               uint32_t bb_format, uint32_t windowed) {
    bool display_ok = adapter_format == D::fmt::x8r8g8b8;
    bool bb_ok = bb_format == D::fmt::x8r8g8b8 || bb_format == D::fmt::a8r8g8b8 || bb_format == D::fmt::unknown;
    return adapter == 0 && type == 1 && display_ok && bb_ok ? D::kOk : D::kNotAvailable;
}

uint32_t d3d_check_device_format(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format,
                                 uint32_t usage, uint32_t rtype, uint32_t format) {
    FormatInfo fi = format_info(format);
    if (usage & 0x100000u) {
        return D::kNotAvailable;
    }
    if (usage & D::usage::depth_stencil) {
        return fi.depth ? D::kOk : D::kNotAvailable;
    }
    if (usage & D::usage::render_target) {
        return format == D::fmt::a8r8g8b8 || format == D::fmt::x8r8g8b8 ? D::kOk : D::kNotAvailable;
    }
    if (rtype == D::rtype::vertex_buffer || rtype == D::rtype::index_buffer) {
        return D::kOk;
    }
    if (rtype == D::rtype::volume_texture || rtype == D::rtype::volume) {
        return D::kNotAvailable;
    }
    if ((usage & D::usage::autogen_mipmap) && fi.compressed) {
        return D::kNotAvailable;
    }
    return fi.texture ? D::kOk : D::kNotAvailable;
}

uint32_t d3d_check_multisample(uint32_t self, uint32_t adapter, uint32_t type, uint32_t format, uint32_t windowed,
                               uint32_t ms, uint32_t quality) {
    if (ms == 0) {
        com_out(quality, 1);
        return D::kOk;
    }
    return D::kNotAvailable;
}

uint32_t d3d_check_depth_stencil_match(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format,
                                       uint32_t rt_format, uint32_t ds_format) {
    return format_info(ds_format).depth ? D::kOk : D::kNotAvailable;
}

uint32_t d3d_check_format_conversion(uint32_t self, uint32_t adapter, uint32_t type, uint32_t src, uint32_t dst) {
    return format_info(src).render_target && format_info(dst).render_target ? D::kOk : D::kNotAvailable;
}

uint32_t d3d_get_device_caps(uint32_t self, uint32_t adapter, uint32_t type, uint32_t out) {
    if (adapter != 0 || out == 0) {
        return D::kInvalidCall;
    }
    fill_caps(out);
    return D::kOk;
}

uint32_t d3d_get_adapter_monitor(uint32_t self, uint32_t adapter) { return 0x00d00001u; }

uint32_t d3d_create_device(uint32_t self, uint32_t adapter, uint32_t type, uint32_t focus, uint32_t behavior,
                           uint32_t params, uint32_t out) {
    if (out == 0 || params == 0) {
        return D::kInvalidCall;
    }
    com_out(out, 0);
    auto d = std::make_unique<Device>();
    d->focus = focus;
    d->behavior = behavior;
    std::memcpy(d->present.data(), mem::ptr(params), sizeof(d->present));
    d->window = d->present[7] ? d->present[7] : focus;
    std::fprintf(stderr,
                 "[D3D9] CreateDevice(adapter=%u, type=%u, behavior=0x%08x%s) requested %ux%u fmt=%u windowed=%u\n",
                 adapter, type, behavior, (behavior & 0x2) ? " FPU_PRESERVE" : " (FPU set to single precision)",
                 d->present[0], d->present[1], d->present[2], d->present[8]);
    PresentOptions options;
    options.capture_dir = settings().capture_dir;
    options.capture_frames = settings().capture_frames;
    options.capture_all = settings().capture_all;
    d->renderer = Renderer::create_metal(options);
    d->d3d = self;
    com_obj_add_ref(self);
    Device* raw = d.get();
    g_device = raw;
    uint32_t guest = com_objects().wrap(g_device_iface, std::move(d));
    auto sc = std::make_unique<SwapChain>();
    sc->device_guest = guest;
    raw->swapchain = com_objects().wrap(g_swapchain_iface, std::move(sc));
    setup_swap_chain(*raw, guest);
    std::memcpy(mem::ptr(params), raw->present.data(), sizeof(raw->present));
    com_out(out, guest);
    return D::kOk;
}

ComInterface g_d3d_iface("d3d9.dll", "IDirect3D9",
                         {
                             com_method<query_interface>("QueryInterface"),
                             com_method<com_obj_add_ref>("AddRef"),
                             com_method<release_any>("Release"),
                             ComMethod{"RegisterSoftwareDevice", 2, nullptr, D::kInvalidCall},
                             com_method<d3d_get_adapter_count>("GetAdapterCount"),
                             com_method<d3d_get_adapter_identifier>("GetAdapterIdentifier"),
                             com_method<d3d_get_adapter_mode_count>("GetAdapterModeCount"),
                             com_method<d3d_enum_adapter_modes>("EnumAdapterModes"),
                             com_method<d3d_get_adapter_display_mode>("GetAdapterDisplayMode"),
                             com_method<d3d_check_device_type>("CheckDeviceType"),
                             com_method<d3d_check_device_format>("CheckDeviceFormat"),
                             com_method<d3d_check_multisample>("CheckDeviceMultiSampleType"),
                             com_method<d3d_check_depth_stencil_match>("CheckDepthStencilMatch"),
                             com_method<d3d_check_format_conversion>("CheckDeviceFormatConversion"),
                             com_method<d3d_get_device_caps>("GetDeviceCaps"),
                             com_method<d3d_get_adapter_monitor>("GetAdapterMonitor"),
                             com_method<d3d_create_device>("CreateDevice"),
                         });

uint32_t direct3d_create9(uint32_t version) {
    std::fprintf(stderr, "[D3D9] Direct3DCreate9(sdk=%u), mode %ux%u@%u\n", version, settings().width,
                 settings().height, settings().refresh);
    return com_objects().wrap(g_d3d_iface, std::make_unique<Direct3D>());
}

constexpr ApiDef kD3d9[] = {
    native_export<Conv::stdcall, direct3d_create9>("d3d9.dll", "Direct3DCreate9"),
};

}

std::span<const ApiDef> apis() { return kD3d9; }

gfx::Renderer* active_renderer() { return g_device ? g_device->renderer.get() : nullptr; }

}
