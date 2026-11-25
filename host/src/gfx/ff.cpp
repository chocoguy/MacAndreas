#include "ff.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace ma::gfx {

namespace {

using namespace d3d;

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }
float bitsf(uint32_t u) { return std::bit_cast<float>(u); }

void transform_row(const float v[4], const Matrix& m, float out[4]) {
    for (int j = 0; j < 4; ++j) {
        out[j] = v[0] * m.m[j] + v[1] * m.m[4 + j] + v[2] * m.m[8 + j] + v[3] * m.m[12 + j];
    }
}

void add_element(VertexLayout& l, uint8_t type, uint8_t usage, uint8_t index, uint16_t& offset) {
    if (l.count >= kMaxElements) {
        return;
    }
    l.elements[l.count++] = VertexElement{0, type, usage, index, offset, 0};
    offset = static_cast<uint16_t>(offset + element_size(type));
}

}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = 0;
            for (int k = 0; k < 4; ++k) {
                v += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = v;
        }
    }
    return r;
}

unsigned element_components(uint8_t type) {
    switch (type) {
    case decltype_::float1: return 1;
    case decltype_::float2:
    case decltype_::short2:
    case decltype_::short2n:
    case decltype_::ushort2n:
    case decltype_::float16_2: return 2;
    case decltype_::float3:
    case decltype_::udec3:
    case decltype_::dec3n: return 3;
    default: return 4;
    }
}

unsigned element_size(uint8_t type) {
    switch (type) {
    case decltype_::float1: return 4;
    case decltype_::float2: return 8;
    case decltype_::float3: return 12;
    case decltype_::float4: return 16;
    case decltype_::d3dcolor:
    case decltype_::ubyte4:
    case decltype_::ubyte4n:
    case decltype_::short2:
    case decltype_::short2n:
    case decltype_::ushort2n:
    case decltype_::udec3:
    case decltype_::dec3n:
    case decltype_::float16_2: return 4;
    case decltype_::short4:
    case decltype_::short4n:
    case decltype_::ushort4n:
    case decltype_::float16_4: return 8;
    default: return 0;
    }
}

VertexLayout layout_from_fvf(uint32_t f) {
    VertexLayout l;
    uint16_t offset = 0;
    uint32_t pos = f & fvf::position_mask;
    if (pos == fvf::xyzrhw) {
        add_element(l, decltype_::float4, declusage::positiont, 0, offset);
        l.pretransformed = 1;
    } else if (pos == fvf::xyzw) {
        add_element(l, decltype_::float4, declusage::position, 0, offset);
    } else if (pos == fvf::xyz) {
        add_element(l, decltype_::float3, declusage::position, 0, offset);
    } else if (pos >= fvf::xyzb1 && pos <= fvf::xyzb5) {
        add_element(l, decltype_::float3, declusage::position, 0, offset);
        unsigned betas = (pos - fvf::xyzrhw) / 2;
        bool last_index = (f & (fvf::lastbeta_ubyte4 | fvf::lastbeta_d3dcolor)) != 0;
        unsigned weights = last_index ? betas - 1 : betas;
        if (weights > 0) {
            static constexpr uint8_t kTypes[] = {decltype_::float1, decltype_::float1, decltype_::float2,
                                                 decltype_::float3, decltype_::float4};
            add_element(l, kTypes[std::min(weights, 4u)], declusage::blendweight, 0, offset);
            if (weights > 4) {
                offset = static_cast<uint16_t>(offset + 4 * (weights - 4));
            }
        }
        if (last_index) {
            uint8_t t = (f & fvf::lastbeta_d3dcolor) ? decltype_::d3dcolor : decltype_::ubyte4;
            add_element(l, t, declusage::blendindices, 0, offset);
        }
    }
    if (f & fvf::normal) {
        add_element(l, decltype_::float3, declusage::normal, 0, offset);
    }
    if (f & fvf::psize) {
        add_element(l, decltype_::float1, declusage::psize, 0, offset);
    }
    if (f & fvf::diffuse) {
        add_element(l, decltype_::d3dcolor, declusage::color, 0, offset);
    }
    if (f & fvf::specular) {
        add_element(l, decltype_::d3dcolor, declusage::color, 1, offset);
    }
    unsigned tex = (f & fvf::texcount_mask) >> fvf::texcount_shift;
    for (unsigned i = 0; i < tex && i < 8; ++i) {
        static constexpr uint8_t kTex[] = {decltype_::float2, decltype_::float3, decltype_::float4,
                                           decltype_::float1};
        add_element(l, kTex[(f >> (16 + 2 * i)) & 3], declusage::texcoord, static_cast<uint8_t>(i), offset);
    }
    return l;
}

uint32_t fvf_stride(uint32_t f) {
    VertexLayout l = layout_from_fvf(f);
    uint32_t end = 0;
    for (unsigned i = 0; i < l.count; ++i) {
        end = std::max<uint32_t>(end, l.elements[i].offset + element_size(l.elements[i].type));
    }
    return end;
}

VertexLayout layout_from_declaration(std::span<const uint8_t> bytes) {
    VertexLayout l;
    for (size_t at = 0; at + 8 <= bytes.size(); at += 8) {
        uint16_t stream;
        uint16_t offset;
        std::memcpy(&stream, bytes.data() + at, 2);
        std::memcpy(&offset, bytes.data() + at + 2, 2);
        uint8_t type = bytes[at + 4];
        uint8_t usage = bytes[at + 6];
        uint8_t index = bytes[at + 7];
        if (stream == 0xff || type == decltype_::unused) {
            break;
        }
        if (l.count >= kMaxElements || stream >= kMaxStreams) {
            continue;
        }
        l.elements[l.count++] = VertexElement{static_cast<uint8_t>(stream), type, usage, index, offset, 0};
        if (usage == declusage::positiont) {
            l.pretransformed = 1;
        }
    }
    return l;
}

const VertexElement* find_element(const VertexLayout& layout, uint8_t usage, uint8_t index) {
    for (unsigned i = 0; i < layout.count; ++i) {
        const auto& e = layout.elements[i];
        if (e.usage == usage && e.index == index) {
            return &e;
        }
    }
    return nullptr;
}

void reset_state(FixedFunctionState& s) {
    s = FixedFunctionState{};
    auto& r = s.rs;
    r[rs::zenable] = 1;
    r[rs::fillmode] = 3;
    r[rs::shademode] = 2;
    r[rs::zwriteenable] = 1;
    r[rs::lastpixel] = 1;
    r[rs::srcblend] = blend::one;
    r[rs::destblend] = blend::zero;
    r[rs::cullmode] = cull::ccw;
    r[rs::zfunc] = cmp::lessequal;
    r[rs::alphafunc] = cmp::always;
    r[rs::fogend] = fbits(1.0f);
    r[rs::fogdensity] = fbits(1.0f);
    r[rs::stencilfail] = 1;
    r[rs::stencilzfail] = 1;
    r[rs::stencilpass] = 1;
    r[rs::stencilfunc] = cmp::always;
    r[rs::stencilmask] = 0xffffffffu;
    r[rs::stencilwritemask] = 0xffffffffu;
    r[rs::texturefactor] = 0xffffffffu;
    r[rs::clipping] = 1;
    r[rs::lighting] = 1;
    r[rs::colorvertex] = 1;
    r[rs::localviewer] = 1;
    r[rs::diffusematerialsource] = 1;
    r[rs::specularmaterialsource] = 2;
    r[rs::pointsize] = fbits(1.0f);
    r[rs::pointsize_min] = fbits(1.0f);
    r[rs::pointsize_max] = fbits(64.0f);
    r[rs::multisampleantialias] = 1;
    r[rs::multisamplemask] = 0xffffffffu;
    r[rs::colorwriteenable] = 0xf;
    r[rs::colorwriteenable1] = 0xf;
    r[rs::colorwriteenable2] = 0xf;
    r[rs::colorwriteenable3] = 0xf;
    r[rs::blendop] = 1;
    r[rs::blendfactor] = 0xffffffffu;
    r[rs::srcblendalpha] = blend::one;
    r[rs::destblendalpha] = blend::zero;
    r[rs::blendopalpha] = 1;
    for (unsigned i = 0; i < kMaxStages; ++i) {
        auto& t = s.tss[i];
        t[tss::colorop] = i == 0 ? top::modulate : top::disable;
        t[tss::colorarg1] = ta::texture;
        t[tss::colorarg2] = ta::current;
        t[tss::alphaop] = i == 0 ? top::selectarg1 : top::disable;
        t[tss::alphaarg1] = ta::texture;
        t[tss::alphaarg2] = ta::current;
        t[tss::texcoordindex] = i;
        t[tss::colorarg0] = ta::current;
        t[tss::alphaarg0] = ta::current;
        t[tss::resultarg] = ta::current;
    }
    for (auto& sm : s.ss) {
        sm[samp::addressu] = 1;
        sm[samp::addressv] = 1;
        sm[samp::addressw] = 1;
        sm[samp::magfilter] = 1;
        sm[samp::minfilter] = 1;
        sm[samp::maxanisotropy] = 1;
    }
}

namespace {

bool arg_is_texture(uint8_t arg) { return (arg & ta::selectmask) == ta::texture; }

bool op_uses_arg0(uint8_t op) { return op == top::multiplyadd || op == top::lerp; }
bool op_uses_arg1(uint8_t op) { return op != top::disable && op != top::selectarg2; }
bool op_uses_arg2(uint8_t op) {
    return op != top::disable && op != top::selectarg1 && op != top::bumpenvmap && op != top::bumpenvmapluminance;
}

bool op_uses_texture(uint8_t op, uint8_t a0, uint8_t a1, uint8_t a2) {
    if (op == top::disable) {
        return false;
    }
    if (op == top::blendtexturealpha || op == top::blendtexturealphapm || op == top::bumpenvmap ||
        op == top::bumpenvmapluminance) {
        return true;
    }
    return (op_uses_arg0(op) && arg_is_texture(a0)) || (op_uses_arg1(op) && arg_is_texture(a1)) ||
           (op_uses_arg2(op) && arg_is_texture(a2));
}

uint8_t material_source(const FixedFunctionState& s, uint32_t state, const VertexLayout& l) {
    uint32_t src = s.rs[state];
    if (!s.rs[rs::colorvertex] || src == 0 || src > 2) {
        return 0;
    }
    return find_element(l, declusage::color, static_cast<uint8_t>(src - 1)) ? static_cast<uint8_t>(src) : 0;
}

void normalize_blend(uint8_t& src, uint8_t& dst) {
    if (src == blend::bothsrcalpha) {
        src = blend::srcalpha;
        dst = blend::invsrcalpha;
    } else if (src == blend::bothinvsrcalpha) {
        src = blend::invsrcalpha;
        dst = blend::srcalpha;
    }
}

}

PipelineKey build_pipeline_key(const FixedFunctionState& s, const VertexLayout& layout,
                               std::span<const uint16_t, kMaxStreams> strides,
                               std::span<const TextureKind, kMaxStages> textures, bool has_color, bool has_depth) {
    PipelineKey key{};
    ShaderKey& k = key.shader;
    k.elements = layout.elements;
    k.element_count = layout.count;
    k.pretransformed = layout.pretransformed;
    bool lighting = s.rs[rs::lighting] != 0 && !layout.pretransformed;
    k.lighting = lighting ? 1 : 0;
    if (lighting) {
        k.color_vertex = s.rs[rs::colorvertex] ? 1 : 0;
        k.normalize_normals = s.rs[rs::normalizenormals] ? 1 : 0;
        k.local_viewer = s.rs[rs::localviewer] ? 1 : 0;
        k.diffuse_source = material_source(s, rs::diffusematerialsource, layout);
        k.ambient_source = material_source(s, rs::ambientmaterialsource, layout);
        k.specular_source = material_source(s, rs::specularmaterialsource, layout);
        k.emissive_source = material_source(s, rs::emissivematerialsource, layout);
        for (unsigned i = 0; i < kMaxLights; ++i) {
            k.light_types[i] = s.light_enabled[i] ? static_cast<uint8_t>(s.lights[i].type) : 0;
        }
    }
    k.specular_enable = s.rs[rs::specularenable] ? 1 : 0;
    if (s.rs[rs::fogenable]) {
        uint32_t table = s.rs[rs::fogtablemode];
        uint32_t vertex = s.rs[rs::fogvertexmode];
        if (table != fog::none) {
            k.fog_source = static_cast<uint8_t>(FogSource::depth);
            k.fog_mode = static_cast<uint8_t>(table);
        } else if (layout.pretransformed || vertex == fog::none) {
            k.fog_source = static_cast<uint8_t>(FogSource::specular_alpha);
        } else {
            k.fog_source = static_cast<uint8_t>(FogSource::depth);
            k.fog_mode = static_cast<uint8_t>(vertex);
            k.range_fog = s.rs[rs::rangefogenable] ? 1 : 0;
        }
    }
    if (s.rs[rs::alphatestenable] && s.rs[rs::alphafunc] != cmp::always) {
        k.alpha_func = static_cast<uint8_t>(s.rs[rs::alphafunc]);
    }
    unsigned count = 0;
    while (count < kMaxStages && s.tss[count][tss::colorop] != top::disable && s.tss[count][tss::colorop] != 0) {
        ++count;
    }
    k.stage_count = static_cast<uint8_t>(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto& t = s.tss[i];
        StageKey& sk = k.stages[i];
        sk.color_op = static_cast<uint8_t>(t[tss::colorop]);
        sk.color_arg0 = op_uses_arg0(sk.color_op) ? static_cast<uint8_t>(t[tss::colorarg0]) : 0;
        sk.color_arg1 = op_uses_arg1(sk.color_op) ? static_cast<uint8_t>(t[tss::colorarg1]) : 0;
        sk.color_arg2 = op_uses_arg2(sk.color_op) ? static_cast<uint8_t>(t[tss::colorarg2]) : 0;
        sk.alpha_op = static_cast<uint8_t>(t[tss::alphaop]);
        if (sk.color_op == top::dotproduct3) {
            sk.alpha_op = top::disable;
        }
        sk.alpha_arg0 = op_uses_arg0(sk.alpha_op) ? static_cast<uint8_t>(t[tss::alphaarg0]) : 0;
        sk.alpha_arg1 = op_uses_arg1(sk.alpha_op) ? static_cast<uint8_t>(t[tss::alphaarg1]) : 0;
        sk.alpha_arg2 = op_uses_arg2(sk.alpha_op) ? static_cast<uint8_t>(t[tss::alphaarg2]) : 0;
        sk.result = t[tss::resultarg] == ta::temp ? 1 : 0;
        bool uses = op_uses_texture(sk.color_op, sk.color_arg0, sk.color_arg1, sk.color_arg2) ||
                    op_uses_texture(sk.alpha_op, sk.alpha_arg0, sk.alpha_arg1, sk.alpha_arg2);
        if (uses && textures[i] != TextureKind::none) {
            sk.texture = static_cast<uint8_t>(textures[i]);
            uint32_t tci = t[tss::texcoordindex];
            sk.coord_index = static_cast<uint8_t>(std::min<uint32_t>(tci & 0xffff, 7));
            sk.coord_mode = static_cast<uint8_t>((tci >> 16) & 0xff);
            if (const auto* e = find_element(layout, declusage::texcoord, sk.coord_index)) {
                sk.coord_count = static_cast<uint8_t>(element_components(e->type));
            }
            uint32_t ttf = t[tss::texturetransformflags];
            sk.transform_count = layout.pretransformed ? 0 : static_cast<uint8_t>(std::min<uint32_t>(ttf & 0xff, 4));
            sk.projected = (ttf & ttff::projected) ? 1 : 0;
        }
    }
    for (unsigned i = 0; i < kMaxStreams; ++i) {
        key.strides[i] = strides[i];
    }
    for (unsigned i = layout.count; i < kMaxElements; ++i) {
        k.elements[i] = VertexElement{};
    }
    key.has_color = has_color ? 1 : 0;
    key.has_depth = has_depth ? 1 : 0;
    key.write_mask = has_color ? static_cast<uint8_t>(s.rs[rs::colorwriteenable] & 0xf) : 0;
    if (s.rs[rs::alphablendenable] && has_color) {
        key.blend_enable = 1;
        key.src_blend = static_cast<uint8_t>(s.rs[rs::srcblend]);
        key.dst_blend = static_cast<uint8_t>(s.rs[rs::destblend]);
        key.blend_op = static_cast<uint8_t>(s.rs[rs::blendop]);
        normalize_blend(key.src_blend, key.dst_blend);
        if (s.rs[rs::separatealphablendenable]) {
            key.src_blend_alpha = static_cast<uint8_t>(s.rs[rs::srcblendalpha]);
            key.dst_blend_alpha = static_cast<uint8_t>(s.rs[rs::destblendalpha]);
            key.blend_op_alpha = static_cast<uint8_t>(s.rs[rs::blendopalpha]);
            normalize_blend(key.src_blend_alpha, key.dst_blend_alpha);
        } else {
            key.src_blend_alpha = key.src_blend;
            key.dst_blend_alpha = key.dst_blend;
            key.blend_op_alpha = key.blend_op;
        }
    }
    return key;
}

void color_to_float4(uint32_t argb, float out[4]) {
    out[0] = static_cast<float>((argb >> 16) & 0xff) / 255.0f;
    out[1] = static_cast<float>((argb >> 8) & 0xff) / 255.0f;
    out[2] = static_cast<float>(argb & 0xff) / 255.0f;
    out[3] = static_cast<float>(argb >> 24) / 255.0f;
}

void build_uniforms(const FixedFunctionState& s, VertexUniforms& vs, FragmentUniforms& fs) {
    Matrix wv = multiply(s.world, s.view);
    Matrix wvp = multiply(wv, s.projection);
    std::memcpy(vs.wvp, wvp.m, sizeof(vs.wvp));
    std::memcpy(vs.wv, wv.m, sizeof(vs.wv));
    for (unsigned i = 0; i < kMaxStages; ++i) {
        std::memcpy(vs.texture[i], s.texture_transform[i].m, sizeof(vs.texture[i]));
    }
    vs.viewport[0] = static_cast<float>(s.viewport.x);
    vs.viewport[1] = static_cast<float>(s.viewport.y);
    vs.viewport[2] = static_cast<float>(std::max<uint32_t>(s.viewport.width, 1));
    vs.viewport[3] = static_cast<float>(std::max<uint32_t>(s.viewport.height, 1));
    vs.depth_range[0] = s.viewport.min_z;
    vs.depth_range[1] = s.viewport.max_z;
    vs.depth_range[2] = 0;
    vs.depth_range[3] = 0;
    color_to_float4(s.rs[rs::ambient], vs.global_ambient);
    std::memcpy(vs.mat_diffuse, s.material.diffuse, 16);
    std::memcpy(vs.mat_ambient, s.material.ambient, 16);
    std::memcpy(vs.mat_specular, s.material.specular, 16);
    std::memcpy(vs.mat_emissive, s.material.emissive, 16);
    vs.mat_power[0] = s.material.power;
    vs.mat_power[1] = vs.mat_power[2] = vs.mat_power[3] = 0;
    for (unsigned i = 0; i < kMaxLights; ++i) {
        const Light& l = s.lights[i];
        LightUniform& u = vs.lights[i];
        std::memcpy(u.diffuse, l.diffuse, 16);
        std::memcpy(u.specular, l.specular, 16);
        std::memcpy(u.ambient, l.ambient, 16);
        float p[4] = {l.position[0], l.position[1], l.position[2], 1};
        transform_row(p, s.view, u.position);
        float d[4] = {l.direction[0], l.direction[1], l.direction[2], 0};
        transform_row(d, s.view, u.direction);
        float len = std::sqrt(u.direction[0] * u.direction[0] + u.direction[1] * u.direction[1] +
                              u.direction[2] * u.direction[2]);
        if (len > 0) {
            for (int c = 0; c < 3; ++c) {
                u.direction[c] /= len;
            }
        }
        u.params0[0] = l.range;
        u.params0[1] = l.falloff;
        u.params0[2] = l.attenuation0;
        u.params0[3] = l.attenuation1;
        u.params1[0] = l.attenuation2;
        u.params1[1] = std::cos(l.theta * 0.5f);
        u.params1[2] = std::cos(l.phi * 0.5f);
        u.params1[3] = 0;
    }
    color_to_float4(s.rs[rs::texturefactor], fs.texture_factor);
    color_to_float4(s.rs[rs::fogcolor], fs.fog_color);
    fs.fog[0] = bitsf(s.rs[rs::fogstart]);
    fs.fog[1] = bitsf(s.rs[rs::fogend]);
    fs.fog[2] = bitsf(s.rs[rs::fogdensity]);
    fs.fog[3] = static_cast<float>(s.rs[rs::alpharef] & 0xff);
    for (unsigned i = 0; i < kMaxStages; ++i) {
        color_to_float4(s.tss[i][tss::constant], fs.stage_constant[i]);
    }
}

uint64_t hash_bytes(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

namespace {

const char* input_type(uint8_t type) {
    switch (type) {
    case decltype_::ubyte4: return "uchar4";
    case decltype_::short2: return "short2";
    case decltype_::short4: return "short4";
    default: return "float4";
    }
}

std::string input_expr(const VertexElement& e, unsigned i) {
    switch (e.type) {
    case decltype_::ubyte4: return std::format("float4(in.a{})", i);
    case decltype_::short2: return std::format("float4(float2(in.a{}), 0.0, 1.0)", i);
    case decltype_::short4: return std::format("float4(in.a{})", i);
    case decltype_::udec3: return std::format("float4(in.a{}.xyz * 1023.0, 1.0)", i);
    default: return std::format("in.a{}", i);
    }
}

std::string cmp_expr(uint8_t func, const std::string& a, const std::string& b) {
    switch (func) {
    case cmp::never: return "false";
    case cmp::less: return a + " < " + b;
    case cmp::equal: return a + " == " + b;
    case cmp::lessequal: return a + " <= " + b;
    case cmp::greater: return a + " > " + b;
    case cmp::notequal: return a + " != " + b;
    case cmp::greaterequal: return a + " >= " + b;
    default: return "true";
    }
}

std::string arg_expr(uint8_t arg, unsigned stage) {
    std::string base;
    switch (arg & ta::selectmask) {
    case ta::diffuse: base = "diffuse"; break;
    case ta::current: base = stage == 0 ? "diffuse" : "cur"; break;
    case ta::texture: base = std::format("tx{}", stage); break;
    case ta::tfactor: base = "u.texture_factor"; break;
    case ta::specular: base = "specular"; break;
    case ta::temp: base = "tmp"; break;
    case ta::constant: base = std::format("u.stage_constant[{}]", stage); break;
    default: base = "diffuse"; break;
    }
    if (arg & ta::complement) {
        base = "(1.0 - " + base + ")";
    }
    if (arg & ta::alphareplicate) {
        base = base + ".aaaa";
    }
    return base;
}

std::string op_expr(uint8_t op, uint8_t a0, uint8_t a1, uint8_t a2, unsigned stage) {
    std::string x0 = arg_expr(a0, stage);
    std::string x1 = arg_expr(a1, stage);
    std::string x2 = arg_expr(a2, stage);
    std::string tex = std::format("tx{}", stage);
    switch (op) {
    case top::selectarg1: return x1;
    case top::selectarg2: return x2;
    case top::modulate: return x1 + " * " + x2;
    case top::modulate2x: return "(" + x1 + " * " + x2 + ") * 2.0";
    case top::modulate4x: return "(" + x1 + " * " + x2 + ") * 4.0";
    case top::add: return x1 + " + " + x2;
    case top::addsigned: return x1 + " + " + x2 + " - 0.5";
    case top::addsigned2x: return "(" + x1 + " + " + x2 + " - 0.5) * 2.0";
    case top::subtract: return x1 + " - " + x2;
    case top::addsmooth: return x1 + " + " + x2 + " - " + x1 + " * " + x2;
    case top::blenddiffusealpha: return "mix(" + x2 + ", " + x1 + ", diffuse.a)";
    case top::blendtexturealpha: return "mix(" + x2 + ", " + x1 + ", " + tex + ".a)";
    case top::blendfactoralpha: return "mix(" + x2 + ", " + x1 + ", u.texture_factor.a)";
    case top::blendtexturealphapm: return x1 + " + " + x2 + " * (1.0 - " + tex + ".a)";
    case top::blendcurrentalpha: return "mix(" + x2 + ", " + x1 + ", " + (stage == 0 ? "diffuse" : "cur") + ".a)";
    case top::premodulate: return x1;
    case top::modulatealpha_addcolor: return "float4(" + x1 + ".rgb + " + x1 + ".a * " + x2 + ".rgb, " + x1 + ".a)";
    case top::modulatecolor_addalpha: return "float4(" + x1 + ".rgb * " + x2 + ".rgb + " + x1 + ".a, " + x1 + ".a)";
    case top::modulateinvalpha_addcolor:
        return "float4((1.0 - " + x1 + ".a) * " + x2 + ".rgb + " + x1 + ".rgb, " + x1 + ".a)";
    case top::modulateinvcolor_addalpha:
        return "float4((1.0 - " + x1 + ".rgb) * " + x2 + ".rgb + " + x1 + ".a, " + x1 + ".a)";
    case top::bumpenvmap:
    case top::bumpenvmapluminance: return tex;
    case top::dotproduct3:
        return "float4(saturate(dot((" + x1 + ".rgb - 0.5) * 2.0, (" + x2 + ".rgb - 0.5) * 2.0)))";
    case top::multiplyadd: return x0 + " + " + x1 + " * " + x2;
    case top::lerp: return "mix(" + x2 + ", " + x1 + ", " + x0 + ")";
    default: return x1;
    }
}

std::string material_expr(uint8_t source, const char* material) {
    switch (source) {
    case 1: return "vdiff";
    case 2: return "vspec";
    default: return std::string("u.") + material;
    }
}

}

std::string generate_msl(const ShaderKey& k) {
    std::string s;
    s += "#include <metal_stdlib>\nusing namespace metal;\n";
    s += "struct LightU { float4 diffuse; float4 specular; float4 ambient; float4 position; float4 direction; "
         "float4 p0; float4 p1; };\n";
    s += "struct VSU { float4x4 wvp; float4x4 wv; float4x4 tex[8]; float4 viewport; float4 depth_range; "
         "float4 global_ambient; float4 mat_diffuse; float4 mat_ambient; float4 mat_specular; float4 mat_emissive; "
         "float4 mat_power; LightU lights[8]; };\n";
    s += "struct FSU { float4 texture_factor; float4 fog_color; float4 fog; float4 stage_constant[8]; };\n";
    s += "struct VIn {\n";
    for (unsigned i = 0; i < k.element_count; ++i) {
        s += std::format("  {} a{} [[attribute({})]];\n", input_type(k.elements[i].type), i, i);
    }
    if (k.element_count == 0) {
        s += "  float4 a0 [[attribute(0)]];\n";
    }
    s += "};\n";
    s += "struct VOut {\n  float4 pos [[position]];\n  float4 diffuse;\n  float4 specular;\n  float fogz;\n";
    for (unsigned i = 0; i < k.stage_count; ++i) {
        if (k.stages[i].texture) {
            s += std::format("  float4 t{};\n", i);
        }
    }
    s += "};\n";

    int pos = -1;
    int nrm = -1;
    int col0 = -1;
    int col1 = -1;
    std::array<int, 8> tex{};
    tex.fill(-1);
    for (unsigned i = 0; i < k.element_count; ++i) {
        const auto& e = k.elements[i];
        if ((e.usage == declusage::position || e.usage == declusage::positiont) && e.index == 0 && pos < 0) {
            pos = static_cast<int>(i);
        } else if (e.usage == declusage::normal && e.index == 0) {
            nrm = static_cast<int>(i);
        } else if (e.usage == declusage::color && e.index == 0) {
            col0 = static_cast<int>(i);
        } else if (e.usage == declusage::color && e.index == 1) {
            col1 = static_cast<int>(i);
        } else if (e.usage == declusage::texcoord && e.index < 8) {
            tex[e.index] = static_cast<int>(i);
        }
    }
    auto element = [&](int idx) { return input_expr(k.elements[static_cast<unsigned>(idx)], static_cast<unsigned>(idx)); };

    s += "vertex VOut vs_main(VIn in [[stage_in]], constant VSU& u [[buffer(8)]]) {\n  VOut o;\n";
    s += std::format("  float4 P = {};\n", pos >= 0 ? element(pos) : "float4(0.0, 0.0, 0.0, 1.0)");
    s += std::format("  float4 vdiff = {};\n", col0 >= 0 ? element(col0) : "float4(1.0)");
    s += std::format("  float4 vspec = {};\n", col1 >= 0 ? element(col1) : "float4(0.0)");
    s += "  float4 vpos = float4(0.0, 0.0, 0.0, 1.0);\n  float3 N = float3(0.0);\n";
    if (k.pretransformed) {
        s += "  float w = P.w != 0.0 ? 1.0 / P.w : 1.0;\n";
        s += "  float2 ndc = (P.xy + 0.5 - u.viewport.xy) / u.viewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);\n";
        s += "  float zr = u.depth_range.y - u.depth_range.x;\n";
        s += "  float z = zr != 0.0 ? (P.z - u.depth_range.x) / zr : P.z;\n";
        s += "  o.pos = float4(ndc * w, z * w, w);\n";
        s += "  o.fogz = w;\n";
    } else {
        s += "  o.pos = u.wvp * P;\n";
        s += "  o.pos.xy += float2(1.0 / u.viewport.z, -1.0 / u.viewport.w) * o.pos.w;\n";
        s += "  vpos = u.wv * P;\n";
        s += std::format("  o.fogz = {};\n", k.range_fog ? "length(vpos.xyz)" : "vpos.z");
        if (nrm >= 0) {
            s += std::format("  N = (u.wv * float4({}.xyz, 0.0)).xyz;\n", element(nrm));
            if (k.normalize_normals) {
                s += "  N = length(N) > 0.0 ? normalize(N) : N;\n";
            }
        }
    }
    if (k.lighting) {
        s += std::format("  float4 mdiff = {};\n", material_expr(k.diffuse_source, "mat_diffuse"));
        s += std::format("  float4 mamb = {};\n", material_expr(k.ambient_source, "mat_ambient"));
        s += std::format("  float4 mspec = {};\n", material_expr(k.specular_source, "mat_specular"));
        s += std::format("  float4 memis = {};\n", material_expr(k.emissive_source, "mat_emissive"));
        s += "  float3 la = float3(0.0);\n  float3 ld = float3(0.0);\n  float3 ls = float3(0.0);\n";
        s += std::format("  float3 V = {};\n", k.local_viewer ? "normalize(-vpos.xyz)" : "float3(0.0, 0.0, -1.0)");
        for (unsigned i = 0; i < kMaxLights; ++i) {
            uint8_t type = k.light_types[i];
            if (type == 0) {
                continue;
            }
            s += "  {\n";
            s += std::format("    LightU L = u.lights[{}];\n    float3 dir;\n    float att = 1.0;\n", i);
            if (type == light::directional) {
                s += "    dir = -L.direction.xyz;\n";
            } else {
                s += "    float3 lv = L.position.xyz - vpos.xyz;\n    float d = length(lv);\n";
                s += "    dir = d > 0.0 ? lv / d : float3(0.0, 0.0, 1.0);\n";
                s += "    float den = L.p0.z + L.p0.w * d + L.p1.x * d * d;\n";
                s += "    att = d > L.p0.x ? 0.0 : (den > 0.0 ? 1.0 / den : 1.0);\n";
                if (type == light::spot) {
                    s += "    float rho = dot(-dir, L.direction.xyz);\n";
                    s += "    float spot = rho > L.p1.y ? 1.0 : (rho <= L.p1.z ? 0.0 : "
                         "pow(max((rho - L.p1.z) / max(L.p1.y - L.p1.z, 1e-6), 0.0), L.p0.y));\n";
                    s += "    att *= spot;\n";
                }
            }
            s += "    float ndl = max(dot(N, dir), 0.0);\n";
            s += "    la += att * L.ambient.rgb;\n    ld += att * ndl * L.diffuse.rgb;\n";
            if (k.specular_enable) {
                s += "    if (ndl > 0.0) { float3 H = normalize(dir + V); "
                     "ls += att * pow(max(dot(N, H), 0.0), u.mat_power.x) * L.specular.rgb; }\n";
            }
            s += "  }\n";
        }
        s += "  o.diffuse = saturate(float4(memis.rgb + mamb.rgb * (u.global_ambient.rgb + la) + mdiff.rgb * ld, "
             "mdiff.a));\n";
        s += "  o.specular = saturate(float4(mspec.rgb * ls, mspec.a));\n";
    } else {
        s += "  o.diffuse = saturate(vdiff);\n  o.specular = saturate(vspec);\n";
    }
    if (k.fog_source == static_cast<uint8_t>(FogSource::specular_alpha)) {
        s += "  o.fogz = vspec.a;\n";
    }
    for (unsigned i = 0; i < k.stage_count; ++i) {
        const StageKey& st = k.stages[i];
        if (!st.texture) {
            continue;
        }
        std::string c;
        switch (st.coord_mode) {
        case 1: c = "float4(N, 1.0)"; break;
        case 2: c = "float4(vpos.xyz, 1.0)"; break;
        case 3:
            c = k.local_viewer ? "float4(reflect(normalize(vpos.xyz), N), 1.0)"
                               : "float4(reflect(float3(0.0, 0.0, 1.0), N), 1.0)";
            break;
        default: {
            int e = tex[st.coord_index];
            if (e < 0) {
                c = "float4(0.0, 0.0, 0.0, 1.0)";
            } else {
                std::string x = element(e);
                switch (st.coord_count) {
                case 1: c = std::format("float4(({}).x, 1.0, 0.0, 0.0)", x); break;
                case 2: c = std::format("float4(({}).xy, 1.0, 0.0)", x); break;
                case 3: c = std::format("float4(({}).xyz, 1.0)", x); break;
                default: c = x; break;
                }
            }
            break;
        }
        }
        s += std::format("  float4 t{} = {};\n", i, c);
        if (st.transform_count) {
            s += std::format("  t{0} = u.tex[{0}] * t{0};\n", i);
        }
        s += std::format("  o.t{0} = t{0};\n", i);
    }
    s += "  return o;\n}\n";

    s += "fragment float4 fs_main(VOut in [[stage_in]], constant FSU& u [[buffer(0)]]";
    for (unsigned i = 0; i < k.stage_count; ++i) {
        if (k.stages[i].texture == static_cast<uint8_t>(TextureKind::tex2d)) {
            s += std::format(", texture2d<float> tex{0} [[texture({0})]], sampler smp{0} [[sampler({0})]]", i);
        } else if (k.stages[i].texture == static_cast<uint8_t>(TextureKind::cube)) {
            s += std::format(", texturecube<float> tex{0} [[texture({0})]], sampler smp{0} [[sampler({0})]]", i);
        }
    }
    s += ") {\n";
    s += "  float4 diffuse = in.diffuse;\n  float4 specular = in.specular;\n";
    s += "  float4 cur = diffuse;\n  float4 tmp = float4(0.0);\n";
    for (unsigned i = 0; i < k.stage_count; ++i) {
        const StageKey& st = k.stages[i];
        if (st.texture == static_cast<uint8_t>(TextureKind::tex2d)) {
            std::string uv = std::format("in.t{}.xy", i);
            if (st.projected) {
                unsigned n = st.transform_count ? st.transform_count : st.coord_count;
                const char* comp = n >= 4 ? "w" : n == 3 ? "z" : "y";
                uv = std::format("(in.t{0}.xy / in.t{0}.{1})", i, comp);
            }
            s += std::format("  float4 tx{0} = tex{0}.sample(smp{0}, {1});\n", i, uv);
        } else if (st.texture == static_cast<uint8_t>(TextureKind::cube)) {
            s += std::format("  float4 tx{0} = tex{0}.sample(smp{0}, in.t{0}.xyz);\n", i);
        } else {
            s += std::format("  float4 tx{} = float4(1.0);\n", i);
        }
        std::string color = op_expr(st.color_op, st.color_arg0, st.color_arg1, st.color_arg2, i);
        std::string alpha;
        if (st.color_op == top::dotproduct3) {
            alpha = "(" + color + ").a";
        } else if (st.alpha_op == top::disable || st.alpha_op == 0) {
            alpha = std::string(i == 0 ? "diffuse" : "cur") + ".a";
        } else {
            alpha = "(" + op_expr(st.alpha_op, st.alpha_arg0, st.alpha_arg1, st.alpha_arg2, i) + ").a";
        }
        s += std::format("  {} = saturate(float4(({}).rgb, {}));\n", st.result ? "tmp" : "cur", color, alpha);
    }
    if (k.specular_enable) {
        s += "  cur.rgb = saturate(cur.rgb + specular.rgb);\n";
    }
    if (k.fog_source == static_cast<uint8_t>(FogSource::specular_alpha)) {
        s += "  float f = saturate(in.fogz);\n  cur.rgb = mix(u.fog_color.rgb, cur.rgb, f);\n";
    } else if (k.fog_source == static_cast<uint8_t>(FogSource::depth)) {
        s += "  float d = in.fogz;\n  float f;\n";
        switch (k.fog_mode) {
        case fog::exp: s += "  f = exp(-d * u.fog.z);\n"; break;
        case fog::exp2: s += "  f = exp(-(d * u.fog.z) * (d * u.fog.z));\n"; break;
        default: s += "  f = u.fog.y != u.fog.x ? (u.fog.y - d) / (u.fog.y - u.fog.x) : 1.0;\n"; break;
        }
        s += "  f = saturate(f);\n  cur.rgb = mix(u.fog_color.rgb, cur.rgb, f);\n";
    }
    if (k.alpha_func) {
        s += std::format("  if (!({})) discard_fragment();\n", cmp_expr(k.alpha_func, "round(cur.a * 255.0)", "u.fog.w"));
    }
    s += "  return cur;\n}\n";
    return s;
}

}
