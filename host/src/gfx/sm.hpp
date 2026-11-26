#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ma::gfx {
struct ShaderKey;
}

namespace ma::gfx::sm {

namespace op {
inline constexpr uint16_t nop = 0;
inline constexpr uint16_t mov = 1;
inline constexpr uint16_t add = 2;
inline constexpr uint16_t sub = 3;
inline constexpr uint16_t mad = 4;
inline constexpr uint16_t mul = 5;
inline constexpr uint16_t rcp = 6;
inline constexpr uint16_t rsq = 7;
inline constexpr uint16_t dp3 = 8;
inline constexpr uint16_t dp4 = 9;
inline constexpr uint16_t min = 10;
inline constexpr uint16_t max = 11;
inline constexpr uint16_t slt = 12;
inline constexpr uint16_t sge = 13;
inline constexpr uint16_t exp = 14;
inline constexpr uint16_t log = 15;
inline constexpr uint16_t lit = 16;
inline constexpr uint16_t dst = 17;
inline constexpr uint16_t lrp = 18;
inline constexpr uint16_t frc = 19;
inline constexpr uint16_t m4x4 = 20;
inline constexpr uint16_t m4x3 = 21;
inline constexpr uint16_t m3x4 = 22;
inline constexpr uint16_t m3x3 = 23;
inline constexpr uint16_t m3x2 = 24;
inline constexpr uint16_t call = 25;
inline constexpr uint16_t callnz = 26;
inline constexpr uint16_t loop = 27;
inline constexpr uint16_t ret = 28;
inline constexpr uint16_t endloop = 29;
inline constexpr uint16_t label = 30;
inline constexpr uint16_t dcl = 31;
inline constexpr uint16_t pow = 32;
inline constexpr uint16_t crs = 33;
inline constexpr uint16_t sgn = 34;
inline constexpr uint16_t abs = 35;
inline constexpr uint16_t nrm = 36;
inline constexpr uint16_t sincos = 37;
inline constexpr uint16_t rep = 38;
inline constexpr uint16_t endrep = 39;
inline constexpr uint16_t if_ = 40;
inline constexpr uint16_t ifc = 41;
inline constexpr uint16_t else_ = 42;
inline constexpr uint16_t endif = 43;
inline constexpr uint16_t break_ = 44;
inline constexpr uint16_t breakc = 45;
inline constexpr uint16_t mova = 46;
inline constexpr uint16_t defb = 47;
inline constexpr uint16_t defi = 48;
inline constexpr uint16_t texcoord = 64;
inline constexpr uint16_t texkill = 65;
inline constexpr uint16_t tex = 66;
inline constexpr uint16_t texbem = 67;
inline constexpr uint16_t texbeml = 68;
inline constexpr uint16_t texreg2ar = 69;
inline constexpr uint16_t texreg2gb = 70;
inline constexpr uint16_t texm3x2pad = 71;
inline constexpr uint16_t texm3x2tex = 72;
inline constexpr uint16_t texm3x3pad = 73;
inline constexpr uint16_t texm3x3tex = 74;
inline constexpr uint16_t texm3x3spec = 76;
inline constexpr uint16_t texm3x3vspec = 77;
inline constexpr uint16_t expp = 78;
inline constexpr uint16_t logp = 79;
inline constexpr uint16_t cnd = 80;
inline constexpr uint16_t def = 81;
inline constexpr uint16_t texreg2rgb = 82;
inline constexpr uint16_t texdp3tex = 83;
inline constexpr uint16_t texm3x2depth = 84;
inline constexpr uint16_t texdp3 = 85;
inline constexpr uint16_t texm3x3 = 86;
inline constexpr uint16_t texdepth = 87;
inline constexpr uint16_t cmp = 88;
inline constexpr uint16_t bem = 89;
inline constexpr uint16_t dp2add = 90;
inline constexpr uint16_t dsx = 91;
inline constexpr uint16_t dsy = 92;
inline constexpr uint16_t texldd = 93;
inline constexpr uint16_t setp = 94;
inline constexpr uint16_t texldl = 95;
inline constexpr uint16_t breakp = 96;
inline constexpr uint16_t phase = 0xfffd;
inline constexpr uint16_t comment = 0xfffe;
inline constexpr uint16_t end = 0xffff;
}

enum class Reg : uint8_t {
    temp = 0,
    input = 1,
    constant = 2,
    addr = 3,
    rastout = 4,
    attrout = 5,
    output = 6,
    constint = 7,
    colorout = 8,
    depthout = 9,
    sampler = 10,
    const2 = 11,
    const3 = 12,
    const4 = 13,
    constbool = 14,
    loop = 15,
    tempfloat16 = 16,
    misctype = 17,
    label = 18,
    predicate = 19,
};

namespace srcmod {
inline constexpr uint8_t none = 0;
inline constexpr uint8_t neg = 1;
inline constexpr uint8_t bias = 2;
inline constexpr uint8_t biasneg = 3;
inline constexpr uint8_t sign = 4;
inline constexpr uint8_t signneg = 5;
inline constexpr uint8_t comp = 6;
inline constexpr uint8_t x2 = 7;
inline constexpr uint8_t x2neg = 8;
inline constexpr uint8_t dz = 9;
inline constexpr uint8_t dw = 10;
inline constexpr uint8_t abs = 11;
inline constexpr uint8_t absneg = 12;
inline constexpr uint8_t not_ = 13;
}

struct Src {
    Reg type = Reg::temp;
    uint16_t num = 0;
    uint8_t swizzle = 0xe4;
    uint8_t mod = 0;
    bool rel = false;
    Reg rel_type = Reg::addr;
    uint16_t rel_num = 0;
    uint8_t rel_comp = 0;
};

struct Dst {
    Reg type = Reg::temp;
    uint16_t num = 0;
    uint8_t mask = 0xf;
    bool saturate = false;
    bool partial = false;
    bool centroid = false;
    int8_t shift = 0;
    bool rel = false;
    Src rel_src{};
};

struct Instr {
    uint16_t opcode = 0;
    uint8_t control = 0;
    bool coissue = false;
    bool predicated = false;
    bool has_dst = false;
    Dst dst{};
    std::array<Src, 4> src{};
    uint8_t src_count = 0;
    Src pred{};
    uint8_t usage = 0;
    uint8_t usage_index = 0;
    uint8_t sampler_type = 0;
    std::array<uint32_t, 4> values{};
};

struct Program {
    bool pixel = false;
    uint8_t major = 0;
    uint8_t minor = 0;
    std::vector<Instr> code;
    std::string error;
    uint32_t const_count = 0;
    bool const_relative = false;
    uint16_t samplers_used = 0;
    uint8_t texcoords_used = 0;
    uint8_t outputs_written = 0;
    bool writes_fog = false;
    bool writes_psize = false;
    bool writes_depth = false;
    std::array<uint8_t, 16> sampler_types{};
    uint64_t hash = 0;

    bool ok() const { return error.empty(); }
    uint32_t version() const { return static_cast<uint32_t>(major) << 8 | minor; }
};

Program parse(std::span<const uint32_t> tokens, uint64_t hash);
std::string disassemble(const Program& p);
const char* opcode_name(uint16_t opcode);

std::string vertex_function(const Program& p, const ShaderKey& key, std::string& error);
std::string fragment_function(const Program& p, const ShaderKey& key, std::string& error);

void register_program(std::shared_ptr<const Program> program);
std::shared_ptr<const Program> find_program(uint64_t hash);

}
