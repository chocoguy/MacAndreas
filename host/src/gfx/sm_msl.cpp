#include "sm.hpp"

#include "ff.hpp"

#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <vector>

namespace ma::gfx::sm {

namespace {

constexpr char kComp[] = "xyzw";

std::string float_lit(float f) {
    if (std::isnan(f)) {
        return "NAN";
    }
    if (std::isinf(f)) {
        return f > 0 ? "INFINITY" : "(-INFINITY)";
    }
    return std::format("as_type<float>({}u)", std::bit_cast<uint32_t>(f));
}

std::string mask_text(uint8_t mask) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        if (mask & (1 << i)) {
            s += kComp[i];
        }
    }
    return s;
}

class Translator {
public:
    Translator(const Program& p, const ShaderKey& k) : p_(p), k_(k) {}

    std::string error;

    std::string vertex() {
        if (p_.pixel) {
            return fail("not a vertex shader");
        }
        if (p_.major >= 3) {
            return fail("vs_3_0 is not supported");
        }
        collect();
        std::string s;
        s += helpers("VSC", 256);
        s += "vertex VOut vs_main(VIn in [[stage_in]], constant VSU& u [[buffer(8)]], constant VSC& C [[buffer(9)]]) {\n";
        s += "  VOut o;\n";
        for (unsigned i = 0; i < kMaxStages; ++i) {
            if ((k_.varyings >> i) & 1) {
                s += std::format("  o.t{} = float4(0.0);\n", i);
            }
        }
        declare_temps(s);
        s += "  int4 a0 = int4(0);\n  int aL = 0;\n";
        s += "  float4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n  float4 oFog = float4(1.0);\n  float4 oPts = float4(0.0);\n";
        s += "  float4 oD0 = float4(0.0);\n  float4 oD1 = float4(0.0);\n";
        for (unsigned i = 0; i < 8; ++i) {
            s += std::format("  float4 oT{} = float4(0.0);\n", i);
        }
        for (const auto& [reg, decl] : inputs_) {
            std::string expr = "float4(0.0, 0.0, 0.0, 1.0)";
            for (unsigned e = 0; e < k_.element_count; ++e) {
                if (k_.elements[e].usage == decl.first && k_.elements[e].index == decl.second) {
                    expr = msl_input_expr(k_.elements[e], e);
                    break;
                }
            }
            s += std::format("  float4 v{} = {};\n", reg, expr);
        }
        if (!body(s)) {
            return {};
        }
        s += "  o.pos = oPos;\n";
        s += "  o.pos.xy += float2(1.0 / u.viewport.z, -1.0 / u.viewport.w) * o.pos.w;\n";
        s += "  o.diffuse = saturate(oD0);\n  o.specular = saturate(oD1);\n";
        if (k_.fog_source == static_cast<uint8_t>(FogSource::depth)) {
            s += "  o.fogz = oPos.w;\n";
        } else {
            s += "  o.fogz = oFog.x;\n";
        }
        for (unsigned i = 0; i < kMaxStages; ++i) {
            if (((k_.varyings >> i) & 1) && ((p_.outputs_written >> i) & 1)) {
                s += std::format("  o.t{0} = oT{0};\n", i);
            }
        }
        s += "  return o;\n}\n";
        return s;
    }

    std::string fragment() {
        if (!p_.pixel) {
            return fail("not a pixel shader");
        }
        if (p_.major >= 3) {
            return fail("ps_3_0 is not supported");
        }
        if (p_.writes_depth) {
            return fail("depth output is not supported");
        }
        collect();
        std::string s;
        s += helpers("PSC", 224);
        s += "fragment float4 fs_main(VOut in [[stage_in]], constant FSU& u [[buffer(0)]], constant PSC& C [[buffer(1)]]";
        for (unsigned i = 0; i < kMaxStages; ++i) {
            uint8_t kind = k_.stages[i].texture;
            if (!((p_.samplers_used >> i) & 1) || kind == 0) {
                continue;
            }
            if (kind == static_cast<uint8_t>(TextureKind::cube)) {
                s += std::format(", texturecube<float> tex{0} [[texture({0})]], sampler smp{0} [[sampler({0})]]", i);
            } else {
                s += std::format(", texture2d<float> tex{0} [[texture({0})]], sampler smp{0} [[sampler({0})]]", i);
            }
        }
        s += ") {\n";
        declare_temps(s);
        s += "  float4 v0 = in.diffuse;\n  float4 v1 = in.specular;\n";
        for (unsigned i = 0; i < kMaxStages; ++i) {
            bool slot = ((k_.varyings >> i) & 1) != 0;
            if (legacy_ps()) {
                s += std::format("  float4 t{} = {};\n", i, slot ? std::format("in.t{}", i) : "float4(0.0)");
            } else if (slot) {
                s += std::format("  float4 tc{0} = in.t{0};\n", i);
            } else {
                s += std::format("  float4 tc{} = float4(0.0);\n", i);
            }
        }
        s += "  float4 oC0 = float4(0.0);\n  int aL = 0;\n";
        if (!body(s)) {
            return {};
        }
        s += std::format("  float4 cur = {};\n", p_.major < 2 ? "r0" : "oC0");
        s += msl_fog_and_alpha(k_);
        s += "  return cur;\n}\n";
        return s;
    }

private:
    const Program& p_;
    const ShaderKey& k_;
    std::map<unsigned, std::pair<uint8_t, uint8_t>> inputs_;
    std::map<unsigned, std::array<uint32_t, 4>> float_defs_;
    std::map<unsigned, std::array<uint32_t, 4>> int_defs_;
    std::map<unsigned, bool> bool_defs_;
    unsigned temps_ = 1;
    int counter_ = 0;
    std::vector<char> blocks_;

    std::string fail(std::string text) {
        if (error.empty()) {
            error = std::move(text);
        }
        return {};
    }

    bool legacy_ps() const { return p_.pixel && p_.version() < 0x104; }

    void collect() {
        auto note_temp = [&](Reg type, unsigned num, unsigned rows) {
            if (type == Reg::temp) {
                temps_ = std::max(temps_, num + rows);
            }
        };
        for (const auto& in : p_.code) {
            if (in.opcode == op::dcl && in.dst.type == Reg::input && !p_.pixel) {
                inputs_[in.dst.num] = {in.usage, in.usage_index};
            } else if (in.opcode == op::def) {
                float_defs_[in.dst.num] = in.values;
            } else if (in.opcode == op::defi) {
                int_defs_[in.dst.num] = in.values;
            } else if (in.opcode == op::defb) {
                bool_defs_[in.dst.num] = in.values[0] != 0;
            }
            if (in.has_dst) {
                note_temp(in.dst.type, in.dst.num, 1);
            }
            for (unsigned i = 0; i < in.src_count; ++i) {
                unsigned rows = 1;
                if (i == 1) {
                    switch (in.opcode) {
                    case op::m4x4:
                    case op::m3x4: rows = 4; break;
                    case op::m4x3:
                    case op::m3x3: rows = 3; break;
                    case op::m3x2: rows = 2; break;
                    default: break;
                    }
                }
                note_temp(in.src[i].type, in.src[i].num, rows);
            }
        }
        if (p_.pixel && p_.major < 2) {
            temps_ = std::max(temps_, 2u);
        }
    }

    void declare_temps(std::string& s) const {
        for (unsigned i = 0; i < temps_; ++i) {
            s += std::format("  float4 r{} = float4(0.0);\n", i);
        }
    }

    std::string helpers(const char* block, unsigned count) const {
        std::string s;
        std::string fn = p_.pixel ? "ps" : "vs";
        s += std::format("static float4 {}_c(constant {}& C, int i) {{\n", fn, block);
        if (!float_defs_.empty()) {
            s += "  switch (i) {\n";
            for (const auto& [n, v] : float_defs_) {
                s += std::format("  case {}: return float4({}, {}, {}, {});\n", n, float_lit(std::bit_cast<float>(v[0])),
                                 float_lit(std::bit_cast<float>(v[1])), float_lit(std::bit_cast<float>(v[2])),
                                 float_lit(std::bit_cast<float>(v[3])));
            }
            s += "  default: break;\n  }\n";
        }
        s += std::format("  return uint(i) < {}u ? C.c[i] : float4(0.0);\n}}\n", count);
        return s;
    }

    std::string const_name() const { return p_.pixel ? "ps_c" : "vs_c"; }

    std::string base(const Src& s, unsigned row) {
        switch (s.type) {
        case Reg::temp: return std::format("r{}", s.num + row);
        case Reg::input:
            if (p_.pixel) {
                return std::format("v{}", s.num & 1);
            }
            if (!inputs_.contains(s.num)) {
                return "float4(0.0, 0.0, 0.0, 1.0)";
            }
            return std::format("v{}", s.num + row);
        case Reg::constant: {
            std::string v;
            if (s.rel) {
                std::string idx;
                if (s.rel_type == Reg::loop) {
                    idx = "aL";
                } else {
                    idx = std::format("a0.{}", kComp[s.rel_comp & 3]);
                }
                v = std::format("{}(C, {} + {})", const_name(), idx, s.num + row);
            } else {
                unsigned n = s.num + row;
                if (auto it = float_defs_.find(n); it != float_defs_.end()) {
                    const auto& d = it->second;
                    v = std::format("float4({}, {}, {}, {})", float_lit(std::bit_cast<float>(d[0])),
                                    float_lit(std::bit_cast<float>(d[1])), float_lit(std::bit_cast<float>(d[2])),
                                    float_lit(std::bit_cast<float>(d[3])));
                } else {
                    v = std::format("C.c[{}]", n);
                }
            }
            if (p_.pixel && p_.major < 2) {
                v = "clamp(" + v + ", -1.0, 1.0)";
            }
            return v;
        }
        case Reg::addr:
            if (p_.pixel) {
                return legacy_ps() ? std::format("t{}", s.num & 7) : std::format("tc{}", s.num & 7);
            }
            return "float4(a0)";
        case Reg::constint: return std::format("float4({})", int_const(s.num));
        case Reg::constbool: return std::format("float4({} ? 1.0 : 0.0)", bool_const(s.num));
        case Reg::loop: return "float4(float(aL))";
        default:
            fail(std::format("unsupported source register type {}", static_cast<int>(s.type)));
            return "float4(0.0)";
        }
    }

    std::string int_const(unsigned n) const {
        if (auto it = int_defs_.find(n); it != int_defs_.end()) {
            const auto& v = it->second;
            return std::format("int4({}, {}, {}, {})", static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]),
                               static_cast<int32_t>(v[2]), static_cast<int32_t>(v[3]));
        }
        return std::format("C.i[{}]", n & 15);
    }

    std::string bool_const(unsigned n) const {
        if (auto it = bool_defs_.find(n); it != bool_defs_.end()) {
            return it->second ? "true" : "false";
        }
        return std::format("(C.b[{}].{} != 0u)", (n & 15) / 4, kComp[n & 3]);
    }

    std::string src(const Src& s, unsigned row = 0) {
        std::string r = base(s, row);
        if (s.swizzle != 0xe4) {
            r = "(" + r + ").";
            for (int i = 0; i < 4; ++i) {
                r += kComp[(s.swizzle >> (2 * i)) & 3];
            }
        }
        switch (s.mod) {
        case srcmod::none: return r;
        case srcmod::neg: return "(-" + r + ")";
        case srcmod::bias: return "(" + r + " - 0.5)";
        case srcmod::biasneg: return "(-(" + r + " - 0.5))";
        case srcmod::sign: return "((" + r + " - 0.5) * 2.0)";
        case srcmod::signneg: return "(-((" + r + " - 0.5) * 2.0))";
        case srcmod::comp: return "(1.0 - " + r + ")";
        case srcmod::x2: return "(" + r + " * 2.0)";
        case srcmod::x2neg: return "(-(" + r + " * 2.0))";
        case srcmod::dz: return "(" + r + " / (" + r + ").z)";
        case srcmod::dw: return "(" + r + " / (" + r + ").w)";
        case srcmod::abs: return "abs(" + r + ")";
        case srcmod::absneg: return "(-abs(" + r + "))";
        default: fail("unsupported source modifier"); return r;
        }
    }

    std::string dst_name(const Dst& d) {
        switch (d.type) {
        case Reg::temp: return std::format("r{}", d.num);
        case Reg::rastout: return d.num == 0 ? "oPos" : d.num == 1 ? "oFog" : "oPts";
        case Reg::attrout: return std::format("oD{}", d.num & 1);
        case Reg::output:
            if (p_.pixel) {
                fail("pixel shader writes an output register");
                return "r0";
            }
            return std::format("oT{}", d.num & 7);
        case Reg::colorout:
            if (d.num != 0) {
                fail("multiple render targets are not supported");
            }
            return "oC0";
        case Reg::addr:
            if (p_.pixel) {
                if (!legacy_ps()) {
                    fail("write to a texture coordinate register");
                }
                return std::format("t{}", d.num & 7);
            }
            return "a0";
        default:
            fail(std::format("unsupported destination register type {}", static_cast<int>(d.type)));
            return "r0";
        }
    }

    void write(std::string& s, const Instr& in, const std::string& value) {
        const Dst& d = in.dst;
        if (d.rel) {
            fail("relative destination addressing is not supported");
            return;
        }
        if (in.predicated) {
            fail("predicated instructions are not supported");
            return;
        }
        std::string name = dst_name(d);
        std::string v = "(" + value + ")";
        if (d.shift > 0) {
            v = std::format("({} * {}.0)", v, 1 << d.shift);
        } else if (d.shift < 0) {
            v = std::format("({} / {}.0)", v, 1 << -d.shift);
        }
        if (d.saturate) {
            v = "saturate(" + v + ")";
        }
        uint8_t mask = d.mask ? d.mask : 0xf;
        std::string m = mask_text(mask);
        if (name == "a0") {
            std::string conv = p_.major < 2 && in.opcode == op::mov ? "int4(floor(" + v + "))" : "int4(floor(" + v + " + 0.5))";
            s += std::format("  a0.{} = ({}).{};\n", m, conv, m);
            return;
        }
        if (mask == 0xf) {
            s += std::format("  {} = {};\n", name, v);
        } else {
            s += std::format("  {}.{} = {}.{};\n", name, m, v, m);
        }
    }

    std::string sample(unsigned stage, const std::string& coord, bool project, const std::string& bias) {
        uint8_t kind = k_.stages[stage & 7].texture;
        if (kind == 0) {
            return "float4(0.0, 0.0, 0.0, 1.0)";
        }
        std::string c = coord;
        if (kind == static_cast<uint8_t>(TextureKind::cube)) {
            if (project) {
                c = std::format("(({0}).xyz / ({0}).w)", coord);
            } else {
                c = std::format("({}).xyz", coord);
            }
        } else if (project) {
            c = std::format("(({0}).xy / ({0}).w)", coord);
        } else {
            c = std::format("({}).xy", coord);
        }
        if (!bias.empty()) {
            return std::format("tex{0}.sample(smp{0}, {1}, bias({2}))", stage & 7, c, bias);
        }
        return std::format("tex{0}.sample(smp{0}, {1})", stage & 7, c);
    }

    std::string legacy_projected_coord(unsigned stage) const {
        const StageKey& st = k_.stages[stage & 7];
        if (!st.projected) {
            return std::format("t{}", stage & 7);
        }
        unsigned n = st.transform_count ? st.transform_count : st.coord_count;
        const char* comp = n >= 4 ? "w" : n == 3 ? "z" : "y";
        return std::format("float4(t{0}.xyz / t{0}.{1}, 1.0)", stage & 7, comp);
    }

    std::string compare(uint8_t control, const std::string& a, const std::string& b) {
        switch (control) {
        case 1: return a + " > " + b;
        case 2: return a + " == " + b;
        case 3: return a + " >= " + b;
        case 4: return a + " < " + b;
        case 5: return a + " != " + b;
        case 6: return a + " <= " + b;
        default: fail("bad comparison"); return "false";
        }
    }

    std::string matrix(const Instr& in, unsigned cols, unsigned rows) {
        std::string a = src(in.src[0]);
        std::string sw = cols == 4 ? "" : ".xyz";
        std::string out = "float4(";
        for (unsigned r = 0; r < 4; ++r) {
            if (r) {
                out += ", ";
            }
            if (r < rows) {
                out += std::format("dot(({}){}, ({}){})", a, sw, src(in.src[1], r), sw);
            } else {
                out += "0.0";
            }
        }
        return out + ")";
    }

    bool body(std::string& s) {
        bool returned = false;
        for (size_t idx = 0; idx < p_.code.size(); ++idx) {
            const Instr& in = p_.code[idx];
            if (returned) {
                if (in.opcode == op::label) {
                    break;
                }
                continue;
            }
            const Src& a = in.src[0];
            switch (in.opcode) {
            case op::nop:
            case op::dcl:
            case op::def:
            case op::defi:
            case op::defb:
            case op::phase: break;
            case op::mov:
            case op::mova: write(s, in, src(a)); break;
            case op::add: write(s, in, src(a) + " + " + src(in.src[1])); break;
            case op::sub: write(s, in, src(a) + " - " + src(in.src[1])); break;
            case op::mul: write(s, in, src(a) + " * " + src(in.src[1])); break;
            case op::mad: write(s, in, src(a) + " * " + src(in.src[1]) + " + " + src(in.src[2])); break;
            case op::rcp: write(s, in, "1.0 / " + src(a)); break;
            case op::rsq: write(s, in, "rsqrt(abs(" + src(a) + "))"); break;
            case op::dp3: write(s, in, std::format("float4(dot(({}).xyz, ({}).xyz))", src(a), src(in.src[1]))); break;
            case op::dp4: write(s, in, std::format("float4(dot({}, {}))", src(a), src(in.src[1]))); break;
            case op::min: write(s, in, std::format("min({}, {})", src(a), src(in.src[1]))); break;
            case op::max: write(s, in, std::format("max({}, {})", src(a), src(in.src[1]))); break;
            case op::slt: write(s, in, std::format("select(float4(0.0), float4(1.0), {} < {})", src(a), src(in.src[1]))); break;
            case op::sge: write(s, in, std::format("select(float4(0.0), float4(1.0), {} >= {})", src(a), src(in.src[1]))); break;
            case op::exp: write(s, in, "exp2(" + src(a) + ")"); break;
            case op::log: write(s, in, "log2(abs(" + src(a) + "))"); break;
            case op::expp:
                if (p_.major < 2) {
                    write(s, in, std::format("float4(exp2(floor(({0}).w)), fract(({0}).w), exp2(({0}).w), 1.0)", src(a)));
                } else {
                    write(s, in, "exp2(" + src(a) + ")");
                }
                break;
            case op::logp:
                if (p_.major < 2) {
                    write(s, in, std::format("float4(floor(log2(abs(({0}).w))), abs(({0}).w) / exp2(floor(log2(abs(({0}).w)))), "
                                             "log2(abs(({0}).w)), 1.0)",
                                             src(a)));
                } else {
                    write(s, in, "log2(abs(" + src(a) + "))");
                }
                break;
            case op::lit:
                write(s, in,
                      std::format("float4(1.0, max(({0}).x, 0.0), ({0}).x > 0.0 && ({0}).y > 0.0 ? "
                                  "pow(({0}).y, clamp(({0}).w, -127.9961, 127.9961)) : 0.0, 1.0)",
                                  src(a)));
                break;
            case op::dst:
                write(s, in, std::format("float4(1.0, ({0}).y * ({1}).y, ({0}).z, ({1}).w)", src(a), src(in.src[1])));
                break;
            case op::lrp: write(s, in, std::format("mix({}, {}, {})", src(in.src[2]), src(in.src[1]), src(a))); break;
            case op::frc: write(s, in, "fract(" + src(a) + ")"); break;
            case op::m4x4: write(s, in, matrix(in, 4, 4)); break;
            case op::m4x3: write(s, in, matrix(in, 4, 3)); break;
            case op::m3x4: write(s, in, matrix(in, 3, 4)); break;
            case op::m3x3: write(s, in, matrix(in, 3, 3)); break;
            case op::m3x2: write(s, in, matrix(in, 3, 2)); break;
            case op::pow: write(s, in, std::format("pow(abs({}), {})", src(a), src(in.src[1]))); break;
            case op::crs:
                write(s, in, std::format("float4(cross(({}).xyz, ({}).xyz), 0.0)", src(a), src(in.src[1])));
                break;
            case op::sgn: write(s, in, "sign(" + src(a) + ")"); break;
            case op::abs: write(s, in, "abs(" + src(a) + ")"); break;
            case op::nrm: write(s, in, std::format("({0}) * rsqrt(dot(({0}).xyz, ({0}).xyz))", src(a))); break;
            case op::sincos: {
                std::string x = src(a);
                write(s, in, std::format("float4(cos(({0}).x), sin(({0}).x), 0.0, 0.0)", x));
                break;
            }
            case op::cnd: write(s, in, std::format("select({}, {}, {} > 0.5)", src(in.src[2]), src(in.src[1]), src(a))); break;
            case op::cmp: write(s, in, std::format("select({}, {}, {} >= 0.0)", src(in.src[2]), src(in.src[1]), src(a))); break;
            case op::dp2add:
                write(s, in, std::format("float4(dot(({}).xy, ({}).xy) + ({}).x)", src(a), src(in.src[1]), src(in.src[2])));
                break;
            case op::dsx: write(s, in, "dfdx(" + src(a) + ")"); break;
            case op::dsy: write(s, in, "dfdy(" + src(a) + ")"); break;
            case op::texcoord:
                if (legacy_ps()) {
                    write(s, in, std::format("float4(saturate(in_t{0}.xyz), 1.0)", in.dst.num & 7));
                } else {
                    write(s, in, src(a));
                }
                break;
            case op::tex:
                if (legacy_ps()) {
                    write(s, in, sample(in.dst.num, legacy_projected_coord(in.dst.num), false, ""));
                } else if (p_.major < 2) {
                    Src plain = a;
                    plain.mod = srcmod::none;
                    std::string c = src(plain);
                    if (a.mod == srcmod::dz) {
                        c = std::format("float4(({0}).xy / ({0}).z, 0.0, 1.0)", c);
                    }
                    write(s, in, sample(in.dst.num, c, a.mod == srcmod::dw, ""));
                } else {
                    unsigned stage = in.src[1].num;
                    bool project = (in.control & 1) != 0;
                    bool bias = (in.control & 2) != 0;
                    std::string c = src(a);
                    std::string r = sample(stage, c, project, bias ? "(" + c + ").w" : "");
                    write(s, in, r + swizzled_sampler(in.src[1]));
                }
                break;
            case op::texldl: {
                unsigned stage = in.src[1].num;
                std::string c = src(a);
                uint8_t kind = k_.stages[stage & 7].texture;
                std::string r = "float4(0.0, 0.0, 0.0, 1.0)";
                if (kind == static_cast<uint8_t>(TextureKind::cube)) {
                    r = std::format("tex{0}.sample(smp{0}, ({1}).xyz, level(({1}).w))", stage & 7, c);
                } else if (kind) {
                    r = std::format("tex{0}.sample(smp{0}, ({1}).xy, level(({1}).w))", stage & 7, c);
                }
                write(s, in, r);
                break;
            }
            case op::texkill: {
                std::string v;
                if (legacy_ps()) {
                    v = std::format("in_t{}", in.dst.num & 7);
                } else {
                    Src r;
                    r.type = in.dst.type;
                    r.num = in.dst.num;
                    v = base(r, 0);
                }
                s += std::format("  if (any(({}).xyz < 0.0)) discard_fragment();\n", v);
                break;
            }
            case op::if_: {
                const Src& b = a;
                s += std::format("  if ({}{}) {{\n", b.mod == srcmod::not_ ? "!" : "", bool_const(b.num));
                blocks_.push_back('i');
                break;
            }
            case op::ifc:
                s += std::format("  if ({}) {{\n", compare(in.control, "(" + src(a) + ").x", "(" + src(in.src[1]) + ").x"));
                blocks_.push_back('i');
                break;
            case op::else_: s += "  } else {\n"; break;
            case op::endif:
                s += "  }\n";
                if (!blocks_.empty()) {
                    blocks_.pop_back();
                }
                break;
            case op::rep: {
                int n = counter_++;
                s += std::format("  for (int rep{0} = 0, rep{0}_n = min({1}.x, 255); rep{0} < rep{0}_n; ++rep{0}) {{\n", n,
                                 int_const(a.num));
                blocks_.push_back('r');
                break;
            }
            case op::endrep:
                s += "  }\n";
                if (!blocks_.empty()) {
                    blocks_.pop_back();
                }
                break;
            case op::loop: {
                int n = counter_++;
                std::string ic = int_const(in.src[1].num);
                s += std::format("  int aL_save{0} = aL;\n  aL = {1}.y;\n  for (int lp{0} = 0, lp{0}_n = min({1}.x, 255); lp{0} < "
                                 "lp{0}_n; ++lp{0}, aL += {1}.z) {{\n",
                                 n, ic);
                blocks_.push_back('l');
                loop_saves_.push_back(n);
                break;
            }
            case op::endloop:
                s += "  }\n";
                if (!loop_saves_.empty()) {
                    s += std::format("  aL = aL_save{};\n", loop_saves_.back());
                    loop_saves_.pop_back();
                }
                if (!blocks_.empty()) {
                    blocks_.pop_back();
                }
                break;
            case op::break_: s += "  break;\n"; break;
            case op::breakc:
                s += std::format("  if ({}) break;\n", compare(in.control, "(" + src(a) + ").x", "(" + src(in.src[1]) + ").x"));
                break;
            case op::ret:
                if (blocks_.empty()) {
                    returned = true;
                } else {
                    fail("ret inside a block is not supported");
                }
                break;
            default: fail(std::format("unsupported instruction {}", opcode_name(in.opcode))); break;
            }
            if (!error.empty()) {
                return false;
            }
        }
        if (!blocks_.empty()) {
            fail("unbalanced control flow");
            return false;
        }
        return true;
    }

    std::string swizzled_sampler(const Src& s) const {
        if (s.swizzle == 0xe4) {
            return "";
        }
        std::string r = ".";
        for (int i = 0; i < 4; ++i) {
            r += kComp[(s.swizzle >> (2 * i)) & 3];
        }
        return r;
    }

    std::vector<int> loop_saves_;
};

}

std::string vertex_function(const Program& p, const ShaderKey& key, std::string& error) {
    Translator t(p, key);
    std::string s = t.vertex();
    error = t.error;
    return t.error.empty() ? s : std::string{};
}

std::string fragment_function(const Program& p, const ShaderKey& key, std::string& error) {
    Translator t(p, key);
    std::string s = t.fragment();
    if (t.error.empty() && p.version() < 0x104) {
        std::string decl;
        for (unsigned i = 0; i < 8; ++i) {
            if (s.find(std::format("in_t{}", i)) != std::string::npos) {
                decl += std::format("  float4 in_t{0} = {1};\n", i, ((key.varyings >> i) & 1) ? std::format("in.t{}", i) : "float4(0.0)");
            }
        }
        if (!decl.empty()) {
            auto at = s.find("  float4 v0 = in.diffuse;");
            s.insert(at, decl);
        }
    }
    error = t.error;
    return t.error.empty() ? s : std::string{};
}

}
