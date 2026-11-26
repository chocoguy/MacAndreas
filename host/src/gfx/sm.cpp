#include "sm.hpp"

#include <bit>
#include <format>
#include <mutex>
#include <unordered_map>

namespace ma::gfx::sm {

namespace {

Reg reg_type(uint32_t t) { return static_cast<Reg>(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

bool has_no_dst(uint16_t o) {
    switch (o) {
    case op::nop:
    case op::call:
    case op::callnz:
    case op::loop:
    case op::ret:
    case op::endloop:
    case op::label:
    case op::rep:
    case op::endrep:
    case op::if_:
    case op::ifc:
    case op::else_:
    case op::endif:
    case op::break_:
    case op::breakc:
    case op::breakp:
    case op::phase: return true;
    default: return false;
    }
}

unsigned matrix_rows(uint16_t o) {
    switch (o) {
    case op::m4x4:
    case op::m3x4: return 4;
    case op::m4x3:
    case op::m3x3: return 3;
    case op::m3x2: return 2;
    default: return 1;
    }
}

struct Reader {
    std::span<const uint32_t> t;
    size_t at = 0;
    size_t end = 0;
    bool fail = false;

    uint32_t next() {
        if (at >= end) {
            fail = true;
            return 0;
        }
        return t[at++];
    }
};

Src read_src(Reader& r, uint8_t major) {
    uint32_t v = r.next();
    Src s;
    s.type = reg_type(v);
    s.num = static_cast<uint16_t>(v & 0x7ff);
    s.swizzle = static_cast<uint8_t>((v >> 16) & 0xff);
    s.mod = static_cast<uint8_t>((v >> 24) & 0xf);
    s.rel = (v & (1u << 13)) != 0;
    if (s.rel) {
        if (major >= 2) {
            uint32_t a = r.next();
            s.rel_type = reg_type(a);
            s.rel_num = static_cast<uint16_t>(a & 0x7ff);
            s.rel_comp = static_cast<uint8_t>((a >> 16) & 3);
        } else {
            s.rel_type = Reg::addr;
            s.rel_num = 0;
            s.rel_comp = 0;
        }
    }
    return s;
}

Dst read_dst(Reader& r, uint8_t major) {
    uint32_t v = r.next();
    Dst d;
    d.type = reg_type(v);
    d.num = static_cast<uint16_t>(v & 0x7ff);
    d.mask = static_cast<uint8_t>((v >> 16) & 0xf);
    uint32_t mods = (v >> 20) & 0xf;
    d.saturate = (mods & 1) != 0;
    d.partial = (mods & 2) != 0;
    d.centroid = (mods & 4) != 0;
    int shift = static_cast<int>((v >> 24) & 0xf);
    d.shift = static_cast<int8_t>(shift >= 8 ? shift - 16 : shift);
    d.rel = (v & (1u << 13)) != 0;
    if (d.rel && major >= 2) {
        uint32_t a = r.next();
        d.rel_src.type = reg_type(a);
        d.rel_src.num = static_cast<uint16_t>(a & 0x7ff);
        d.rel_src.rel_comp = static_cast<uint8_t>((a >> 16) & 3);
    }
    return d;
}

void note_usage(Program& p, const Instr& in) {
    for (unsigned i = 0; i < in.src_count; ++i) {
        const Src& s = in.src[i];
        if (s.type == Reg::constant) {
            unsigned rows = i == 1 ? matrix_rows(in.opcode) : 1;
            p.const_count = std::max<uint32_t>(p.const_count, s.num + rows);
            if (s.rel) {
                p.const_relative = true;
            }
        } else if (s.type == Reg::sampler) {
            p.samplers_used |= static_cast<uint16_t>(1u << (s.num & 15));
        } else if (p.pixel && s.type == Reg::addr && p.major >= 1) {
            if (p.version() >= 0x104 || p.major >= 2) {
                p.texcoords_used |= static_cast<uint8_t>(1u << (s.num & 7));
            }
        }
    }
    if (in.has_dst) {
        const Dst& d = in.dst;
        if (p.pixel) {
            if (d.type == Reg::depthout) {
                p.writes_depth = true;
            }
            if (p.version() < 0x104 && d.type == Reg::addr &&
                (in.opcode == op::tex || in.opcode == op::texcoord || (in.opcode >= op::texbem && in.opcode <= op::texm3x3vspec) ||
                 in.opcode == op::texreg2rgb || in.opcode == op::texdp3tex || in.opcode == op::texm3x2depth ||
                 in.opcode == op::texdp3 || in.opcode == op::texm3x3)) {
                p.texcoords_used |= static_cast<uint8_t>(1u << (d.num & 7));
                if (in.opcode != op::texcoord && in.opcode != op::texm3x2pad && in.opcode != op::texm3x3pad &&
                    in.opcode != op::texdp3 && in.opcode != op::texm3x3 && in.opcode != op::texm3x2depth) {
                    p.samplers_used |= static_cast<uint16_t>(1u << (d.num & 7));
                }
            }
            if (p.version() == 0x104 && in.opcode == op::tex) {
                p.samplers_used |= static_cast<uint16_t>(1u << (d.num & 7));
            }
        } else {
            if (d.type == Reg::output) {
                p.outputs_written |= static_cast<uint8_t>(1u << (d.num & 7));
            } else if (d.type == Reg::rastout && d.num == 1) {
                p.writes_fog = true;
            } else if (d.type == Reg::rastout && d.num == 2) {
                p.writes_psize = true;
            }
        }
    }
}

}

Program parse(std::span<const uint32_t> tokens, uint64_t hash) {
    Program p;
    p.hash = hash;
    if (tokens.empty()) {
        p.error = "empty shader";
        return p;
    }
    uint32_t version = tokens[0];
    uint32_t kind = version & 0xffff0000u;
    if (kind != 0xffff0000u && kind != 0xfffe0000u) {
        p.error = std::format("bad version token {:08x}", version);
        return p;
    }
    p.pixel = kind == 0xffff0000u;
    p.major = static_cast<uint8_t>((version >> 8) & 0xff);
    p.minor = static_cast<uint8_t>(version & 0xff);
    size_t i = 1;
    bool ended = false;
    while (i < tokens.size()) {
        uint32_t t = tokens[i];
        uint16_t opcode = static_cast<uint16_t>(t & 0xffff);
        if (opcode == op::end) {
            ended = true;
            break;
        }
        if (opcode == op::comment) {
            i += 1 + ((t >> 16) & 0x7fff);
            continue;
        }
        size_t len = 0;
        if (p.major >= 2) {
            len = (t >> 24) & 0xf;
        } else if (opcode == op::def) {
            len = 5;
        } else {
            while (i + 1 + len < tokens.size() && (tokens[i + 1 + len] & 0x80000000u) != 0) {
                ++len;
            }
        }
        if (i + 1 + len > tokens.size()) {
            p.error = std::format("instruction at token {} runs past the end", i);
            return p;
        }
        Instr in;
        in.opcode = opcode;
        in.control = static_cast<uint8_t>((t >> 16) & 0xff);
        in.coissue = (t & (1u << 30)) != 0;
        in.predicated = (t & (1u << 28)) != 0;
        Reader r{tokens, i + 1, i + 1 + len};
        if (opcode == op::dcl) {
            uint32_t u = r.next();
            in.usage = static_cast<uint8_t>(u & 0x1f);
            in.usage_index = static_cast<uint8_t>((u >> 16) & 0xf);
            in.sampler_type = static_cast<uint8_t>((u >> 27) & 0xf);
            in.has_dst = true;
            in.dst = read_dst(r, p.major);
            if (in.dst.type == Reg::sampler) {
                p.sampler_types[in.dst.num & 15] = in.sampler_type;
            }
        } else if (opcode == op::def || opcode == op::defi) {
            in.has_dst = true;
            in.dst = read_dst(r, p.major);
            for (auto& v : in.values) {
                v = r.next();
            }
        } else if (opcode == op::defb) {
            in.has_dst = true;
            in.dst = read_dst(r, p.major);
            in.values[0] = r.next();
        } else {
            in.has_dst = !has_no_dst(opcode);
            if (in.has_dst) {
                in.dst = read_dst(r, p.major);
            }
            if (in.predicated) {
                in.pred = read_src(r, p.major);
            }
            while (r.at < r.end && in.src_count < in.src.size()) {
                in.src[in.src_count++] = read_src(r, p.major);
            }
        }
        if (r.fail) {
            p.error = std::format("truncated operands for {} at token {}", opcode_name(opcode), i);
            return p;
        }
        note_usage(p, in);
        p.code.push_back(in);
        i += 1 + len;
    }
    if (!ended) {
        p.error = "missing end token";
    }
    return p;
}

const char* opcode_name(uint16_t o) {
    switch (o) {
    case op::nop: return "nop";
    case op::mov: return "mov";
    case op::add: return "add";
    case op::sub: return "sub";
    case op::mad: return "mad";
    case op::mul: return "mul";
    case op::rcp: return "rcp";
    case op::rsq: return "rsq";
    case op::dp3: return "dp3";
    case op::dp4: return "dp4";
    case op::min: return "min";
    case op::max: return "max";
    case op::slt: return "slt";
    case op::sge: return "sge";
    case op::exp: return "exp";
    case op::log: return "log";
    case op::lit: return "lit";
    case op::dst: return "dst";
    case op::lrp: return "lrp";
    case op::frc: return "frc";
    case op::m4x4: return "m4x4";
    case op::m4x3: return "m4x3";
    case op::m3x4: return "m3x4";
    case op::m3x3: return "m3x3";
    case op::m3x2: return "m3x2";
    case op::call: return "call";
    case op::callnz: return "callnz";
    case op::loop: return "loop";
    case op::ret: return "ret";
    case op::endloop: return "endloop";
    case op::label: return "label";
    case op::dcl: return "dcl";
    case op::pow: return "pow";
    case op::crs: return "crs";
    case op::sgn: return "sgn";
    case op::abs: return "abs";
    case op::nrm: return "nrm";
    case op::sincos: return "sincos";
    case op::rep: return "rep";
    case op::endrep: return "endrep";
    case op::if_: return "if";
    case op::ifc: return "ifc";
    case op::else_: return "else";
    case op::endif: return "endif";
    case op::break_: return "break";
    case op::breakc: return "breakc";
    case op::mova: return "mova";
    case op::defb: return "defb";
    case op::defi: return "defi";
    case op::texcoord: return "texcoord";
    case op::texkill: return "texkill";
    case op::tex: return "tex";
    case op::texbem: return "texbem";
    case op::texbeml: return "texbeml";
    case op::texreg2ar: return "texreg2ar";
    case op::texreg2gb: return "texreg2gb";
    case op::texm3x2pad: return "texm3x2pad";
    case op::texm3x2tex: return "texm3x2tex";
    case op::texm3x3pad: return "texm3x3pad";
    case op::texm3x3tex: return "texm3x3tex";
    case op::texm3x3spec: return "texm3x3spec";
    case op::texm3x3vspec: return "texm3x3vspec";
    case op::expp: return "expp";
    case op::logp: return "logp";
    case op::cnd: return "cnd";
    case op::def: return "def";
    case op::texreg2rgb: return "texreg2rgb";
    case op::texdp3tex: return "texdp3tex";
    case op::texm3x2depth: return "texm3x2depth";
    case op::texdp3: return "texdp3";
    case op::texm3x3: return "texm3x3";
    case op::texdepth: return "texdepth";
    case op::cmp: return "cmp";
    case op::bem: return "bem";
    case op::dp2add: return "dp2add";
    case op::dsx: return "dsx";
    case op::dsy: return "dsy";
    case op::texldd: return "texldd";
    case op::setp: return "setp";
    case op::texldl: return "texldl";
    case op::breakp: return "breakp";
    case op::phase: return "phase";
    default: return "unknown";
    }
}

namespace {

std::string reg_name(const Program& p, Reg type, uint16_t num) {
    switch (type) {
    case Reg::temp: return std::format("r{}", num);
    case Reg::input: return std::format("v{}", num);
    case Reg::constant: return std::format("c{}", num);
    case Reg::addr: return p.pixel ? std::format("t{}", num) : std::format("a{}", num);
    case Reg::rastout: return num == 0 ? "oPos" : num == 1 ? "oFog" : "oPts";
    case Reg::attrout: return std::format("oD{}", num);
    case Reg::output: return p.major >= 3 ? std::format("o{}", num) : std::format("oT{}", num);
    case Reg::constint: return std::format("i{}", num);
    case Reg::colorout: return std::format("oC{}", num);
    case Reg::depthout: return "oDepth";
    case Reg::sampler: return std::format("s{}", num);
    case Reg::constbool: return std::format("b{}", num);
    case Reg::loop: return "aL";
    case Reg::misctype: return num == 0 ? "vPos" : "vFace";
    case Reg::label: return std::format("l{}", num);
    case Reg::predicate: return "p0";
    default: return std::format("?{}_{}", static_cast<int>(type), num);
    }
}

std::string swizzle_text(uint8_t sw) {
    if (sw == 0xe4) {
        return "";
    }
    static constexpr char kC[] = "xyzw";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) {
        s += kC[(sw >> (2 * i)) & 3];
    }
    if (s[1] == s[2] && s[2] == s[3] && s[3] == s[4]) {
        s.resize(2);
    }
    return s;
}

std::string src_text(const Program& p, const Src& s) {
    std::string r = reg_name(p, s.type, s.num);
    if (s.rel) {
        static constexpr char kC[] = "xyzw";
        r = std::format("{}[{}.{} + {}]", r.substr(0, 1), reg_name(p, s.rel_type, s.rel_num), kC[s.rel_comp & 3],
                        s.num);
    }
    r += swizzle_text(s.swizzle);
    switch (s.mod) {
    case srcmod::neg: return "-" + r;
    case srcmod::bias: return r + "_bias";
    case srcmod::biasneg: return "-" + r + "_bias";
    case srcmod::sign: return r + "_bx2";
    case srcmod::signneg: return "-" + r + "_bx2";
    case srcmod::comp: return "1-" + r;
    case srcmod::x2: return r + "_x2";
    case srcmod::x2neg: return "-" + r + "_x2";
    case srcmod::dz: return r + "_dz";
    case srcmod::dw: return r + "_dw";
    case srcmod::abs: return r + "_abs";
    case srcmod::absneg: return "-" + r + "_abs";
    case srcmod::not_: return "!" + r;
    default: return r;
    }
}

std::string dst_text(const Program& p, const Dst& d) {
    std::string r = reg_name(p, d.type, d.num);
    if (d.mask != 0xf) {
        r += ".";
        for (int i = 0; i < 4; ++i) {
            if (d.mask & (1 << i)) {
                r += "xyzw"[i];
            }
        }
    }
    return r;
}

const char* usage_name(uint8_t u) {
    static constexpr const char* kNames[] = {"position", "blendweight", "blendindices", "normal", "psize",
                                             "texcoord", "tangent",     "binormal",     "tessfactor", "positiont",
                                             "color",    "fog",         "depth",        "sample"};
    return u < std::size(kNames) ? kNames[u] : "usage?";
}

}

std::string disassemble(const Program& p) {
    std::string s = std::format("{}_{}_{}\n", p.pixel ? "ps" : "vs", p.major, p.minor);
    if (!p.ok()) {
        s += "; error: " + p.error + "\n";
    }
    for (const auto& in : p.code) {
        std::string line = in.coissue ? "+" : "";
        line += opcode_name(in.opcode);
        if (in.opcode == op::dcl) {
            if (in.dst.type == Reg::sampler) {
                static constexpr const char* kT[] = {"", "", "2d", "cube", "volume"};
                line += std::format("_{}", in.sampler_type < 5 ? kT[in.sampler_type] : "?");
            } else if (!(p.pixel && p.major < 3)) {
                line += std::format("_{}{}", usage_name(in.usage), in.usage_index ? std::to_string(in.usage_index) : "");
            }
        }
        if (in.has_dst) {
            if (in.dst.saturate) {
                line += "_sat";
            }
            if (in.dst.partial) {
                line += "_pp";
            }
            if (in.dst.centroid) {
                line += "_centroid";
            }
            if (in.dst.shift) {
                line += in.dst.shift > 0 ? std::format("_x{}", 1 << in.dst.shift) : std::format("_d{}", 1 << -in.dst.shift);
            }
        }
        if (in.control && in.opcode != op::dcl && in.opcode != op::def) {
            line += std::format("_ctl{}", in.control);
        }
        line += " ";
        if (in.predicated) {
            line += "(" + src_text(p, in.pred) + ") ";
        }
        bool first = true;
        if (in.has_dst) {
            line += dst_text(p, in.dst);
            first = false;
        }
        if (in.opcode == op::def) {
            line += std::format(", {}, {}, {}, {}", std::bit_cast<float>(in.values[0]), std::bit_cast<float>(in.values[1]),
                                std::bit_cast<float>(in.values[2]), std::bit_cast<float>(in.values[3]));
        } else if (in.opcode == op::defi) {
            line += std::format(", {}, {}, {}, {}", static_cast<int32_t>(in.values[0]), static_cast<int32_t>(in.values[1]),
                                static_cast<int32_t>(in.values[2]), static_cast<int32_t>(in.values[3]));
        } else if (in.opcode == op::defb) {
            line += std::format(", {}", in.values[0] ? "true" : "false");
        }
        for (unsigned i = 0; i < in.src_count; ++i) {
            line += (first ? "" : ", ") + src_text(p, in.src[i]);
            first = false;
        }
        s += line + "\n";
    }
    return s;
}

namespace {

std::mutex g_registry_mutex;
std::unordered_map<uint64_t, std::shared_ptr<const Program>>& registry() {
    static std::unordered_map<uint64_t, std::shared_ptr<const Program>> m;
    return m;
}

}

void register_program(std::shared_ptr<const Program> program) {
    std::lock_guard lock(g_registry_mutex);
    registry().emplace(program->hash, std::move(program));
}

std::shared_ptr<const Program> find_program(uint64_t hash) {
    std::lock_guard lock(g_registry_mutex);
    auto it = registry().find(hash);
    return it == registry().end() ? nullptr : it->second;
}

}
