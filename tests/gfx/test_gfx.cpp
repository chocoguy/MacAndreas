#include "check.hpp"

#include "gfx/d3d9_defs.hpp"
#include "gfx/ff.hpp"
#include "gfx/formats.hpp"
#include "gfx/renderer.hpp"
#include "gfx/sm.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ma::gfx;
namespace D = ma::gfx::d3d;

namespace {

std::array<TextureKind, kMaxStages> one_texture() {
    std::array<TextureKind, kMaxStages> t{};
    t[0] = TextureKind::tex2d;
    return t;
}

std::array<uint16_t, kMaxStreams> stride0(uint16_t s) {
    std::array<uint16_t, kMaxStreams> a{};
    a[0] = s;
    return a;
}

void test_fvf_layouts() {
    uint32_t im2d = D::fvf::xyzrhw | D::fvf::diffuse | (1u << D::fvf::texcount_shift);
    VertexLayout l = layout_from_fvf(im2d);
    CHECK(l.count == 3);
    CHECK(l.pretransformed == 1);
    CHECK(l.elements[0].usage == D::declusage::positiont && l.elements[0].offset == 0);
    CHECK(l.elements[1].usage == D::declusage::color && l.elements[1].offset == 16);
    CHECK(l.elements[2].usage == D::declusage::texcoord && l.elements[2].offset == 20);
    CHECK(fvf_stride(im2d) == 28);

    uint32_t lit = D::fvf::xyz | D::fvf::normal | D::fvf::diffuse | D::fvf::specular | (2u << D::fvf::texcount_shift) |
                   (1u << 18);
    VertexLayout m = layout_from_fvf(lit);
    CHECK(m.count == 6);
    CHECK(m.pretransformed == 0);
    CHECK(find_element(m, D::declusage::color, 1)->offset == 28);
    CHECK(find_element(m, D::declusage::texcoord, 1)->type == D::decltype_::float3);
    CHECK(fvf_stride(lit) == 52);

    uint32_t skinned = D::fvf::xyzb5 | D::fvf::lastbeta_ubyte4 | D::fvf::normal;
    VertexLayout k = layout_from_fvf(skinned);
    CHECK(find_element(k, D::declusage::blendweight, 0)->type == D::decltype_::float4);
    CHECK(find_element(k, D::declusage::blendindices, 0)->type == D::decltype_::ubyte4);
    CHECK(fvf_stride(skinned) == 12 + 16 + 4 + 12);
}

void test_declaration_layout() {
    const uint8_t decl[] = {
        0, 0, 0, 0,  D::decltype_::float3,   0, D::declusage::position, 0,
        1, 0, 0, 0,  D::decltype_::d3dcolor, 0, D::declusage::color,    0,
        0, 0, 12, 0, D::decltype_::short2,   0, D::declusage::texcoord, 0,
        0xff, 0, 0, 0, D::decltype_::unused, 0, 0,                      0,
    };
    VertexLayout l = layout_from_declaration(decl);
    CHECK(l.count == 3);
    CHECK(l.elements[1].stream == 1);
    CHECK(l.elements[2].offset == 12);
    CHECK(l.pretransformed == 0);
}

void test_pipeline_key() {
    FixedFunctionState s;
    reset_state(s);
    VertexLayout l = layout_from_fvf(D::fvf::xyzrhw | D::fvf::diffuse | (1u << D::fvf::texcount_shift));
    auto key = build_pipeline_key(s, l, stride0(28), one_texture(), true, true);
    CHECK(key.shader.stage_count == 1);
    CHECK(key.shader.stages[0].color_op == D::top::modulate);
    CHECK(key.shader.stages[0].texture == static_cast<uint8_t>(TextureKind::tex2d));
    CHECK(key.shader.stages[0].coord_count == 2);
    CHECK(key.shader.lighting == 0);
    CHECK(key.blend_enable == 0);
    CHECK(key.write_mask == 0xf);

    auto same = build_pipeline_key(s, l, stride0(28), one_texture(), true, true);
    CHECK(hash_key(key) == hash_key(same));

    s.rs[D::rs::texturefactor] = 0x80ff0000u;
    s.rs[D::rs::alpharef] = 100;
    auto uniform_only = build_pipeline_key(s, l, stride0(28), one_texture(), true, true);
    CHECK(hash_key(uniform_only) == hash_key(key));

    s.tss[0][D::tss::colorop] = D::top::selectarg2;
    s.tss[0][D::tss::alphaop] = D::top::selectarg2;
    auto untextured = build_pipeline_key(s, l, stride0(28), one_texture(), true, true);
    CHECK(untextured.shader.stages[0].texture == 0);
    CHECK(untextured.shader.stages[0].color_arg1 == 0);

    s.rs[D::rs::alphablendenable] = 1;
    s.rs[D::rs::srcblend] = D::blend::bothsrcalpha;
    auto blended = build_pipeline_key(s, l, stride0(28), one_texture(), true, true);
    CHECK(blended.blend_enable == 1);
    CHECK(blended.src_blend == D::blend::srcalpha && blended.dst_blend == D::blend::invsrcalpha);
    CHECK(blended.src_blend_alpha == D::blend::srcalpha);

    s.rs[D::rs::alphatestenable] = 1;
    s.rs[D::rs::alphafunc] = D::cmp::greater;
    auto tested = build_pipeline_key(s, l, stride0(28), one_texture(), true, false);
    CHECK(tested.shader.alpha_func == D::cmp::greater);
    CHECK(tested.has_depth == 0);
}

void test_lighting_key_and_uniforms() {
    FixedFunctionState s;
    reset_state(s);
    VertexLayout l = layout_from_fvf(D::fvf::xyz | D::fvf::normal | (1u << D::fvf::texcount_shift));
    s.light_enabled[0] = 1;
    s.lights[0].type = D::light::directional;
    s.light_enabled[2] = 1;
    s.lights[2].type = D::light::point;
    auto key = build_pipeline_key(s, l, stride0(32), one_texture(), true, true);
    CHECK(key.shader.lighting == 1);
    CHECK(key.shader.light_types[0] == D::light::directional);
    CHECK(key.shader.light_types[1] == 0);
    CHECK(key.shader.light_types[2] == D::light::point);
    CHECK(key.shader.diffuse_source == 0);

    s.view.m[12] = 5;
    s.lights[2].position[0] = 1;
    s.rs[D::rs::ambient] = 0xff336699u;
    VertexUniforms vs{};
    FragmentUniforms fs{};
    build_uniforms(s, vs, fs);
    CHECK(vs.lights[2].position[0] == 6.0f);
    CHECK(vs.wvp[12] == 5.0f);
    CHECK(vs.global_ambient[0] > 0.19f && vs.global_ambient[0] < 0.21f);
    CHECK(fs.texture_factor[3] == 1.0f);
}

void test_formats() {
    CHECK(level_pitch(D::fmt::dxt1, 256) == 512);
    CHECK(level_pitch(D::fmt::dxt5, 2) == 16);
    CHECK(level_rows(D::fmt::dxt3, 1) == 1);
    CHECK(level_pitch(D::fmt::r5g6b5, 3) == 8);
    CHECK(full_mip_count(256, 64) == 9);
    CHECK(format_info(D::fmt::dxt5).gpu == GpuFormat::bc3);
    CHECK(!format_info(D::fmt::p8).texture);
    CHECK(!needs_conversion(D::fmt::a8r8g8b8));
    CHECK(needs_conversion(D::fmt::x8r8g8b8));

    const uint16_t src[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
    uint8_t out[16] = {};
    convert_to_bgra8(D::fmt::r5g6b5, reinterpret_cast<const uint8_t*>(src), 8, 4, 1, out, 16);
    CHECK(out[2] == 255 && out[1] == 0 && out[0] == 0 && out[3] == 255);
    CHECK(out[5] == 255 && out[6] == 0);
    CHECK(out[8] == 255 && out[10] == 0);
    CHECK(out[12] == 255 && out[13] == 255 && out[14] == 255);

    const uint16_t argb1555[2] = {0x7c00, 0x801f};
    convert_to_bgra8(D::fmt::a1r5g5b5, reinterpret_cast<const uint8_t*>(argb1555), 4, 2, 1, out, 8);
    CHECK(out[2] == 255 && out[3] == 0);
    CHECK(out[4] == 255 && out[7] == 255);

    const uint16_t argb4444 = 0x8f00;
    convert_to_bgra8(D::fmt::a4r4g4b4, reinterpret_cast<const uint8_t*>(&argb4444), 2, 1, 1, out, 4);
    CHECK(out[2] == 255 && out[3] == 136);

    const uint8_t xrgb[4] = {1, 2, 3, 0};
    convert_to_bgra8(D::fmt::x8r8g8b8, xrgb, 4, 1, 1, out, 4);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 255);

    const uint8_t la[2] = {200, 50};
    convert_to_bgra8(D::fmt::a8l8, la, 2, 1, 1, out, 4);
    CHECK(out[0] == 200 && out[1] == 200 && out[2] == 200 && out[3] == 50);
}

bool msl_compiles(const ShaderKey& key) {
    std::string error = compile_msl(generate_msl(key));
    if (!error.empty()) {
        std::fprintf(stderr, "MSL error: %s\n%s\n", error.c_str(), generate_msl(key).c_str());
    }
    return error.empty();
}

void test_generated_msl() {
    if (compile_msl("#include <metal_stdlib>\nvertex float4 vs_main() { return float4(0); }\n"
                    "fragment float4 fs_main() { return float4(0); }\n")
            .starts_with("no Metal device")) {
        std::fprintf(stdout, "test_gfx: no Metal device, MSL compile checks skipped\n");
        return;
    }
    FixedFunctionState s;
    reset_state(s);
    VertexLayout im2d = layout_from_fvf(D::fvf::xyzrhw | D::fvf::diffuse | (1u << D::fvf::texcount_shift));
    CHECK(msl_compiles(build_pipeline_key(s, im2d, stride0(28), one_texture(), true, true).shader));

    s.rs[D::rs::alphatestenable] = 1;
    s.rs[D::rs::alphafunc] = D::cmp::greaterequal;
    s.rs[D::rs::fogenable] = 1;
    s.rs[D::rs::fogtablemode] = D::fog::linear;
    s.rs[D::rs::specularenable] = 1;
    s.light_enabled[0] = s.light_enabled[1] = s.light_enabled[2] = 1;
    s.lights[0].type = D::light::directional;
    s.lights[1].type = D::light::point;
    s.lights[2].type = D::light::spot;
    s.tss[0][D::tss::texcoordindex] = D::tci::cameraspacereflectionvector;
    s.tss[0][D::tss::texturetransformflags] = 3 | D::ttff::projected;
    s.tss[1][D::tss::colorop] = D::top::lerp;
    s.tss[1][D::tss::colorarg0] = D::ta::tfactor | D::ta::alphareplicate;
    s.tss[1][D::tss::colorarg1] = D::ta::texture | D::ta::complement;
    s.tss[1][D::tss::alphaop] = D::top::blendcurrentalpha;
    s.tss[1][D::tss::resultarg] = D::ta::temp;
    s.tss[2][D::tss::colorop] = D::top::dotproduct3;
    s.tss[2][D::tss::colorarg1] = D::ta::temp;
    s.tss[2][D::tss::colorarg2] = D::ta::specular;
    s.rs[D::rs::colorvertex] = 1;
    s.rs[D::rs::diffusematerialsource] = 1;
    VertexLayout lit = layout_from_fvf(D::fvf::xyz | D::fvf::normal | D::fvf::diffuse | D::fvf::specular |
                                       (2u << D::fvf::texcount_shift));
    std::array<TextureKind, kMaxStages> textures{};
    textures[0] = TextureKind::cube;
    textures[1] = TextureKind::tex2d;
    auto key = build_pipeline_key(s, lit, stride0(48), textures, true, true);
    CHECK(key.shader.stage_count == 3);
    CHECK(key.shader.diffuse_source == 1);
    CHECK(msl_compiles(key.shader));

    const uint8_t decl[] = {
        0, 0, 0, 0,  D::decltype_::short4,  0, D::declusage::position, 0,
        0, 0, 8, 0,  D::decltype_::ubyte4,  0, D::declusage::texcoord, 0,
        0, 0, 12, 0, D::decltype_::short2,  0, D::declusage::texcoord, 1,
        0, 0, 16, 0, D::decltype_::float16_2, 0, D::declusage::normal, 0,
        0xff, 0, 0, 0, D::decltype_::unused, 0, 0,                     0,
    };
    FixedFunctionState plain;
    reset_state(plain);
    plain.rs[D::rs::lighting] = 0;
    plain.tss[0][D::tss::texcoordindex] = 1;
    CHECK(msl_compiles(
        build_pipeline_key(plain, layout_from_declaration(decl), stride0(20), one_texture(), true, false).shader));
}

namespace tok {

using ma::gfx::sm::Reg;

uint32_t ins(uint16_t opcode, uint32_t len, uint32_t control = 0) { return opcode | (control << 16) | (len << 24); }

uint32_t reg_bits(Reg r) {
    uint32_t t = static_cast<uint32_t>(r);
    return ((t & 7) << 28) | ((t & 0x18) << 8);
}

uint32_t dst(Reg r, uint32_t num, uint32_t mask = 0xf, uint32_t mods = 0) {
    return 0x80000000u | reg_bits(r) | (mask << 16) | (mods << 20) | num;
}

uint32_t src(Reg r, uint32_t num, uint32_t swizzle = 0xe4, uint32_t mod = 0, bool rel = false) {
    return 0x80000000u | reg_bits(r) | (swizzle << 16) | (mod << 24) | (rel ? 1u << 13 : 0) | num;
}

uint32_t usage(uint32_t u, uint32_t index = 0) { return 0x80000000u | u | (index << 16); }

constexpr uint32_t kXXXX = 0x00;
constexpr uint32_t kYYYY = 0x55;
constexpr uint32_t kWWWW = 0xff;

}

std::shared_ptr<const sm::Program> register_tokens(std::vector<uint32_t> t) {
    uint64_t h = hash_bytes(t.data(), t.size() * 4);
    auto p = std::make_shared<sm::Program>(sm::parse(t, h));
    sm::register_program(p);
    return p;
}

std::shared_ptr<const sm::Program> skin_vs() {
    using namespace tok;
    namespace o = ma::gfx::sm::op;
    std::vector<uint32_t> t = {
        0xfffe0201,
        ins(o::dcl, 2), usage(D::declusage::position), dst(Reg::input, 0),
        ins(o::dcl, 2), usage(D::declusage::blendweight), dst(Reg::input, 1),
        ins(o::dcl, 2), usage(D::declusage::blendindices), dst(Reg::input, 2),
        ins(o::dcl, 2), usage(D::declusage::normal), dst(Reg::input, 3),
        ins(o::dcl, 2), usage(D::declusage::texcoord), dst(Reg::input, 5),
        ins(o::def, 5), dst(Reg::constant, 30), std::bit_cast<uint32_t>(0.5f), 0, 0, std::bit_cast<uint32_t>(1.0f),
        ins(o::mova, 2), dst(Reg::addr, 0, 0x3), src(Reg::input, 2),
        ins(o::mul, 4), dst(Reg::temp, 8), src(Reg::input, 1, kXXXX), src(Reg::constant, 8, 0xe4, 0, true), 0x80000000u | (3u << 28) | (0u << 16),
        ins(o::mul, 4), dst(Reg::temp, 9), src(Reg::input, 1, kXXXX), src(Reg::constant, 9, 0xe4, 0, true), 0x80000000u | (3u << 28) | (0u << 16),
        ins(o::mul, 4), dst(Reg::temp, 10), src(Reg::input, 1, kXXXX), src(Reg::constant, 10, 0xe4, 0, true), 0x80000000u | (3u << 28) | (1u << 16),
        ins(o::m4x3, 3), dst(Reg::temp, 3, 0x7), src(Reg::input, 0), src(Reg::temp, 8),
        ins(o::m3x3, 4), dst(Reg::temp, 5, 0x7), src(Reg::input, 3), src(Reg::constant, 8, 0xe4, 0, true), 0x80000000u | (3u << 28),
        ins(o::mov, 2), dst(Reg::temp, 3, 0x8), src(Reg::input, 0),
        ins(o::m4x4, 3), dst(Reg::rastout, 0), src(Reg::temp, 3), src(Reg::constant, 0),
        ins(o::dp4, 3), dst(Reg::temp, 0, 0x8), src(Reg::temp, 3), src(Reg::constant, 3),
        ins(o::mad, 4), dst(Reg::rastout, 1), src(Reg::temp, 0, kWWWW), src(Reg::constant, 7, kXXXX), src(Reg::constant, 7, kYYYY),
        ins(o::rsq, 2), dst(Reg::temp, 5, 0x8), src(Reg::temp, 5, kWWWW),
        ins(o::max, 3), dst(Reg::temp, 6, 0x1), src(Reg::constant, 30, kWWWW), src(Reg::temp, 5, kXXXX, ma::gfx::sm::srcmod::neg),
        ins(o::mad, 4), dst(Reg::attrout, 0), src(Reg::constant, 6), src(Reg::temp, 6, kXXXX), src(Reg::constant, 4),
        ins(o::mov, 2), dst(Reg::output, 0, 0x3), src(Reg::input, 5),
        0x0000ffff,
    };
    return register_tokens(std::move(t));
}

std::shared_ptr<const sm::Program> combine_ps() {
    using namespace tok;
    namespace o = ma::gfx::sm::op;
    std::vector<uint32_t> t = {
        0xffff0101,
        ins(o::tex, 0), dst(Reg::addr, 0),
        ins(o::tex, 0), dst(Reg::addr, 1),
        ins(o::mul, 0), dst(Reg::temp, 0, 0x7), src(Reg::addr, 0), src(Reg::addr, 0, kWWWW, ma::gfx::sm::srcmod::comp),
        ins(o::mov, 0) | (1u << 30), dst(Reg::temp, 0, 0x8), src(Reg::addr, 0, kWWWW),
        ins(o::mul, 0), dst(Reg::temp, 0), src(Reg::temp, 0), src(Reg::input, 0),
        ins(o::mul, 0), dst(Reg::temp, 1), src(Reg::addr, 1), src(Reg::addr, 1, kWWWW),
        ins(o::mad, 0), dst(Reg::temp, 0, 0x7), src(Reg::temp, 1), src(Reg::constant, 0), src(Reg::temp, 0),
        0x0000ffff,
    };
    return register_tokens(std::move(t));
}

void test_shader_parse() {
    auto vs = skin_vs();
    CHECK(vs->ok());
    CHECK(!vs->pixel && vs->major == 2 && vs->minor == 1);
    CHECK(vs->const_relative);
    CHECK(vs->writes_fog);
    CHECK(vs->outputs_written == 1);
    CHECK(sm::disassemble(*vs).find("c[a0.x + 8]") != std::string::npos);

    auto ps = combine_ps();
    CHECK(ps->ok());
    CHECK(ps->pixel && ps->version() == 0x101);
    CHECK(ps->samplers_used == 3);
    CHECK(ps->texcoords_used == 3);
    CHECK(ps->code[3].coissue);
    CHECK(ps->const_count == 1);

    std::vector<uint32_t> truncated = {0xfffe0200, tok::ins(sm::op::mov, 2), tok::dst(sm::Reg::temp, 0)};
    CHECK(!sm::parse(truncated, 1).ok());
    std::vector<uint32_t> bad = {0x12345678, 0x0000ffff};
    CHECK(!sm::parse(bad, 2).ok());
}

void test_shader_msl() {
    if (compile_msl("#include <metal_stdlib>\nvertex float4 vs_main() { return float4(0); }\n"
                    "fragment float4 fs_main() { return float4(0); }\n")
            .starts_with("no Metal device")) {
        return;
    }
    auto vs = skin_vs();
    auto ps = combine_ps();
    const uint8_t decl[] = {
        0, 0, 0,  0, D::decltype_::float3, 0, D::declusage::position,     0,
        0, 0, 12, 0, D::decltype_::float3, 0, D::declusage::blendweight,  0,
        0, 0, 24, 0, D::decltype_::ubyte4, 0, D::declusage::blendindices, 0,
        0, 0, 28, 0, D::decltype_::float3, 0, D::declusage::normal,       0,
        0, 0, 40, 0, D::decltype_::float2, 0, D::declusage::texcoord,     0,
        0xff, 0, 0, 0, D::decltype_::unused, 0, 0,                        0,
    };
    VertexLayout layout = layout_from_declaration(decl);
    FixedFunctionState s;
    reset_state(s);
    s.rs[D::rs::fogenable] = 1;
    s.rs[D::rs::alphatestenable] = 1;
    s.rs[D::rs::alphafunc] = D::cmp::greater;

    auto skin_ff = build_pipeline_key(s, layout, stride0(48), one_texture(), true, true, vs.get(), nullptr);
    CHECK(skin_ff.shader.vs_program == vs->hash);
    CHECK(skin_ff.shader.lighting == 0);
    CHECK(skin_ff.shader.fog_source == static_cast<uint8_t>(FogSource::specular_alpha));
    CHECK((skin_ff.shader.varyings & 1) != 0);
    std::string src = generate_msl(skin_ff.shader);
    CHECK(src.find("vs_c(C, a0.x + 8)") != std::string::npos);
    CHECK(compile_msl(src).empty());

    std::array<TextureKind, kMaxStages> two{};
    two[0] = TextureKind::tex2d;
    two[1] = TextureKind::cube;
    VertexLayout lit = layout_from_fvf(D::fvf::xyz | D::fvf::normal | D::fvf::diffuse | (2u << D::fvf::texcount_shift));
    auto ff_ps = build_pipeline_key(s, lit, stride0(40), two, true, true, nullptr, ps.get());
    CHECK(ff_ps.shader.stage_count == 2);
    CHECK(ff_ps.shader.stages[1].texture == static_cast<uint8_t>(TextureKind::cube));
    CHECK(ff_ps.shader.varyings == 3);
    std::string fsrc = generate_msl(ff_ps.shader);
    CHECK(fsrc.find("clamp(C.c[0], -1.0, 1.0)") != std::string::npos);
    CHECK(compile_msl(fsrc).empty());

    auto both = build_pipeline_key(s, layout, stride0(48), two, true, true, vs.get(), ps.get());
    CHECK(both.shader.varyings == 3);
    CHECK(compile_msl(generate_msl(both.shader)).empty());

    std::array<TextureKind, kMaxStages> none{};
    auto unbound = build_pipeline_key(s, lit, stride0(40), none, true, false, nullptr, ps.get());
    CHECK(unbound.shader.stages[0].texture == 0);
    CHECK(compile_msl(generate_msl(unbound.shader)).empty());

    ShaderKey missing = skin_ff.shader;
    missing.vs_program = 0x1234;
    CHECK(generate_msl(missing).empty());
}

}

int main() {
    test_fvf_layouts();
    test_declaration_layout();
    test_pipeline_key();
    test_lighting_key_and_uniforms();
    test_formats();
    test_generated_msl();
    test_shader_parse();
    test_shader_msl();
    return ma::test::finish("test_gfx");
}
