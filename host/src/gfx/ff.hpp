#pragma once

#include "d3d9_defs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace ma::gfx {

inline constexpr unsigned kMaxStages = 8;
inline constexpr unsigned kMaxSamplers = 16;
inline constexpr unsigned kMaxStreams = 8;
inline constexpr unsigned kMaxElements = 16;
inline constexpr unsigned kMaxLights = 8;
inline constexpr unsigned kStoredLights = 16;

struct Matrix {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Matrix multiply(const Matrix& a, const Matrix& b);

struct Light {
    uint32_t type = d3d::light::directional;
    float diffuse[4] = {1, 1, 1, 0};
    float specular[4] = {};
    float ambient[4] = {};
    float position[3] = {};
    float direction[3] = {0, 0, 1};
    float range = 0;
    float falloff = 0;
    float attenuation0 = 0;
    float attenuation1 = 0;
    float attenuation2 = 0;
    float theta = 0;
    float phi = 0;
};
static_assert(sizeof(Light) == 104);

struct Material {
    float diffuse[4] = {};
    float ambient[4] = {};
    float specular[4] = {};
    float emissive[4] = {};
    float power = 0;
};
static_assert(sizeof(Material) == 68);

struct Viewport {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float min_z = 0;
    float max_z = 1;
};
static_assert(sizeof(Viewport) == 24);

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

struct VertexElement {
    uint8_t stream = 0;
    uint8_t type = 0;
    uint8_t usage = 0;
    uint8_t index = 0;
    uint16_t offset = 0;
    uint16_t pad = 0;
};

struct VertexLayout {
    std::array<VertexElement, kMaxElements> elements{};
    uint8_t count = 0;
    uint8_t pretransformed = 0;
    uint16_t pad = 0;
};

VertexLayout layout_from_fvf(uint32_t fvf);
uint32_t fvf_stride(uint32_t fvf);
VertexLayout layout_from_declaration(std::span<const uint8_t> elements);
unsigned element_components(uint8_t type);
unsigned element_size(uint8_t type);
const VertexElement* find_element(const VertexLayout& layout, uint8_t usage, uint8_t index);

struct FixedFunctionState {
    std::array<uint32_t, d3d::rs::count> rs{};
    std::array<std::array<uint32_t, d3d::tss::count>, kMaxStages> tss{};
    std::array<std::array<uint32_t, d3d::samp::count>, kMaxSamplers> ss{};
    std::array<Light, kStoredLights> lights{};
    std::array<uint8_t, kStoredLights> light_enabled{};
    Material material{};
    Matrix world{};
    Matrix view{};
    Matrix projection{};
    std::array<Matrix, kMaxStages> texture_transform{};
    Viewport viewport{};
    Rect scissor{};
};

void reset_state(FixedFunctionState& s);

enum class TextureKind : uint8_t { none = 0, tex2d = 1, cube = 2 };

struct StageKey {
    uint8_t color_op = 0;
    uint8_t color_arg0 = 0;
    uint8_t color_arg1 = 0;
    uint8_t color_arg2 = 0;
    uint8_t alpha_op = 0;
    uint8_t alpha_arg0 = 0;
    uint8_t alpha_arg1 = 0;
    uint8_t alpha_arg2 = 0;
    uint8_t result = 0;
    uint8_t coord_index = 0;
    uint8_t coord_mode = 0;
    uint8_t coord_count = 0;
    uint8_t transform_count = 0;
    uint8_t projected = 0;
    uint8_t texture = 0;
    uint8_t pad = 0;
};

struct ShaderKey {
    uint64_t vs_program = 0;
    uint64_t ps_program = 0;
    std::array<VertexElement, kMaxElements> elements{};
    uint8_t element_count = 0;
    uint8_t pretransformed = 0;
    uint8_t lighting = 0;
    uint8_t color_vertex = 0;
    uint8_t normalize_normals = 0;
    uint8_t local_viewer = 0;
    uint8_t specular_enable = 0;
    uint8_t diffuse_source = 0;
    uint8_t ambient_source = 0;
    uint8_t specular_source = 0;
    uint8_t emissive_source = 0;
    uint8_t fog_source = 0;
    uint8_t fog_mode = 0;
    uint8_t range_fog = 0;
    uint8_t alpha_func = 0;
    uint8_t stage_count = 0;
    std::array<uint8_t, kMaxLights> light_types{};
    std::array<StageKey, kMaxStages> stages{};
    uint8_t varyings = 0;
    uint8_t pad[7] = {};
};
static_assert(std::has_unique_object_representations_v<ShaderKey>);

struct PipelineKey {
    ShaderKey shader{};
    std::array<uint16_t, kMaxStreams> strides{};
    uint8_t blend_enable = 0;
    uint8_t src_blend = 0;
    uint8_t dst_blend = 0;
    uint8_t blend_op = 0;
    uint8_t src_blend_alpha = 0;
    uint8_t dst_blend_alpha = 0;
    uint8_t blend_op_alpha = 0;
    uint8_t write_mask = 0;
    uint8_t has_depth = 0;
    uint8_t has_color = 0;
    uint8_t pad[6] = {};
};
static_assert(std::has_unique_object_representations_v<PipelineKey>);

enum class FogSource : uint8_t { none = 0, depth = 1, specular_alpha = 2 };

struct LightUniform {
    float diffuse[4];
    float specular[4];
    float ambient[4];
    float position[4];
    float direction[4];
    float params0[4];
    float params1[4];
};

struct VertexUniforms {
    float wvp[16];
    float wv[16];
    float texture[kMaxStages][16];
    float viewport[4];
    float depth_range[4];
    float global_ambient[4];
    float mat_diffuse[4];
    float mat_ambient[4];
    float mat_specular[4];
    float mat_emissive[4];
    float mat_power[4];
    LightUniform lights[kMaxLights];
};

struct FragmentUniforms {
    float texture_factor[4];
    float fog_color[4];
    float fog[4];
    float stage_constant[kMaxStages][4];
};

namespace sm {
struct Program;
}

PipelineKey build_pipeline_key(const FixedFunctionState& s, const VertexLayout& layout,
                               std::span<const uint16_t, kMaxStreams> strides,
                               std::span<const TextureKind, kMaxStages> textures, bool has_color, bool has_depth,
                               const sm::Program* vs = nullptr, const sm::Program* ps = nullptr);

void build_uniforms(const FixedFunctionState& s, VertexUniforms& vs, FragmentUniforms& fs);

uint64_t hash_bytes(const void* data, size_t size);

template <typename T>
uint64_t hash_key(const T& key) {
    return hash_bytes(&key, sizeof(T));
}

std::string generate_msl(const ShaderKey& key);
std::string msl_input_expr(const VertexElement& e, unsigned index);
std::string msl_fog_and_alpha(const ShaderKey& key);
std::string msl_compare(uint8_t func, const std::string& a, const std::string& b);

void color_to_float4(uint32_t argb, float out[4]);

}
