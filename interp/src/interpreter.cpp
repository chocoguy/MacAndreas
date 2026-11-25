#include "macandreas/interp.hpp"

#include "guest_memory.hpp"

#include <Zydis/Zydis.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace ma::interp {

namespace {

using host::GuestState;

struct Flags {
    static void set_szp(GuestState& s, uint32_t r, unsigned bits) {
        uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
        r &= mask;
        s.aflag.zf = r == 0;
        s.aflag.sf = (r >> (bits - 1)) & 1;
        s.aflag.pf = (std::popcount(r & 0xffu) & 1) == 0;
    }
};

uint32_t mask_of(unsigned bits) { return bits == 32 ? 0xffffffffu : (1u << bits) - 1; }

uint32_t sign_bit(unsigned bits) { return 1u << (bits - 1); }

int32_t sign_extend(uint32_t v, unsigned bits) {
    if (bits == 32) {
        return static_cast<int32_t>(v);
    }
    uint32_t m = sign_bit(bits);
    v &= mask_of(bits);
    return static_cast<int32_t>((v ^ m) - m);
}

class Machine {
public:
    Machine(GuestState& s, const Hooks& hooks) : s_(s), hooks_(hooks) {
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    }

    Result run(uint32_t pc) {
        Result result;
        bool first = true;
        for (;;) {
            if (!first && hooks_.has_native && hooks_.has_native(hooks_.context, pc)) {
                host::eip(s_) = pc;
                hooks_.call_native(hooks_.context, s_, pc);
                result.stop = Stop::handed_off;
                result.pc = pc;
                return result;
            }
            if (!decode(pc)) {
                result.stop = Stop::unsupported;
                result.pc = pc;
                result.message = std::format("cannot decode instruction at 0x{:08x}", pc);
                return result;
            }
            host::eip(s_) = pc;
            next_ = pc + insn_.length;
            ++result.steps;
            Outcome o = execute();
            if (o == Outcome::unsupported) {
                result.stop = Stop::unsupported;
                result.pc = pc;
                result.message = std::format("unsupported instruction at 0x{:08x}: {}", pc, describe(pc));
                return result;
            }
            if (o == Outcome::returned) {
                result.stop = Stop::returned;
                result.pc = host::eip(s_);
                return result;
            }
            pc = next_;
            first = false;
        }
    }

private:
    enum class Outcome { next, returned, unsupported };

    bool decode(uint32_t pc) {
        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, host::mem::ptr(pc), 15, &insn_, ops_));
    }

    uint32_t& gpr32(unsigned index) {
        switch (index) {
        case 0: return s_.gpr.rax.dword;
        case 1: return s_.gpr.rcx.dword;
        case 2: return s_.gpr.rdx.dword;
        case 3: return s_.gpr.rbx.dword;
        case 4: return s_.gpr.rsp.dword;
        case 5: return s_.gpr.rbp.dword;
        case 6: return s_.gpr.rsi.dword;
        default: return s_.gpr.rdi.dword;
        }
    }

    uint32_t reg_read(ZydisRegister r) {
        if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) {
            return gpr32(r - ZYDIS_REGISTER_EAX);
        }
        if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) {
            return gpr32(r - ZYDIS_REGISTER_AX) & 0xffffu;
        }
        if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BL) {
            return gpr32(r - ZYDIS_REGISTER_AL) & 0xffu;
        }
        if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) {
            return (gpr32(r - ZYDIS_REGISTER_AH) >> 8) & 0xffu;
        }
        if (r >= ZYDIS_REGISTER_SPL && r <= ZYDIS_REGISTER_DIL) {
            return gpr32(r - ZYDIS_REGISTER_SPL + 4) & 0xffu;
        }
        switch (r) {
        case ZYDIS_REGISTER_CS: return 0x1b;
        case ZYDIS_REGISTER_FS: return 0x3b;
        case ZYDIS_REGISTER_DS:
        case ZYDIS_REGISTER_ES:
        case ZYDIS_REGISTER_SS:
        case ZYDIS_REGISTER_GS: return 0x23;
        default: break;
        }
        bad_ = true;
        return 0;
    }

    void reg_write(ZydisRegister r, uint32_t v) {
        if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) {
            gpr32(r - ZYDIS_REGISTER_EAX) = v;
        } else if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) {
            uint32_t& g = gpr32(r - ZYDIS_REGISTER_AX);
            g = (g & 0xffff0000u) | (v & 0xffffu);
        } else if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BL) {
            uint32_t& g = gpr32(r - ZYDIS_REGISTER_AL);
            g = (g & 0xffffff00u) | (v & 0xffu);
        } else if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) {
            uint32_t& g = gpr32(r - ZYDIS_REGISTER_AH);
            g = (g & 0xffff00ffu) | ((v & 0xffu) << 8);
        } else if (r == ZYDIS_REGISTER_DS || r == ZYDIS_REGISTER_ES || r == ZYDIS_REGISTER_SS ||
                   r == ZYDIS_REGISTER_GS || r == ZYDIS_REGISTER_FS) {
        } else {
            bad_ = true;
        }
    }

    uint32_t address(const ZydisDecodedOperand& op) {
        uint32_t a = static_cast<uint32_t>(op.mem.disp.value);
        if (op.mem.base != ZYDIS_REGISTER_NONE) {
            a += reg_read(op.mem.base);
        }
        if (op.mem.index != ZYDIS_REGISTER_NONE) {
            a += reg_read(op.mem.index) * op.mem.scale;
        }
        if (op.mem.segment == ZYDIS_REGISTER_FS) {
            a += s_.addr.fs_base.dword;
        } else if (op.mem.segment == ZYDIS_REGISTER_GS) {
            a += s_.addr.gs_base.dword;
        }
        return a;
    }

    uint32_t mem_read(uint32_t a, unsigned bits) {
        switch (bits) {
        case 8: return host::mem::read<uint8_t>(a);
        case 16: return host::mem::read<uint16_t>(a);
        default: return host::mem::read<uint32_t>(a);
        }
    }

    void mem_write(uint32_t a, unsigned bits, uint32_t v) {
        switch (bits) {
        case 8: host::mem::write<uint8_t>(a, static_cast<uint8_t>(v)); break;
        case 16: host::mem::write<uint16_t>(a, static_cast<uint16_t>(v)); break;
        default: host::mem::write<uint32_t>(a, v); break;
        }
    }

    unsigned width(unsigned i) const { return ops_[i].size; }

    uint32_t read(unsigned i) {
        const auto& op = ops_[i];
        switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            return reg_read(op.reg.value);
        case ZYDIS_OPERAND_TYPE_MEMORY:
            return mem_read(address(op), op.size);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            return static_cast<uint32_t>(op.imm.value.u) & mask_of(op.size ? op.size : 32);
        default:
            bad_ = true;
            return 0;
        }
    }

    uint32_t read_as(unsigned i, unsigned bits) {
        const auto& op = ops_[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            return static_cast<uint32_t>(op.imm.is_signed ? static_cast<uint64_t>(op.imm.value.s) : op.imm.value.u) &
                   mask_of(bits);
        }
        return read(i) & mask_of(bits);
    }

    void write(unsigned i, uint32_t v) {
        const auto& op = ops_[i];
        switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            reg_write(op.reg.value, v);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            mem_write(address(op), op.size, v);
            break;
        default:
            bad_ = true;
        }
    }

    void push(uint32_t v) {
        host::esp(s_) -= 4;
        host::mem::write<uint32_t>(host::esp(s_), v);
    }

    uint32_t pop() {
        uint32_t v = host::mem::read<uint32_t>(host::esp(s_));
        host::esp(s_) += 4;
        return v;
    }

    uint32_t branch_target(unsigned i) {
        const auto& op = ops_[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            ZyanU64 target = 0;
            ZydisCalcAbsoluteAddress(&insn_, &op, host::eip(s_), &target);
            return static_cast<uint32_t>(target);
        }
        return read(i);
    }

    bool condition(unsigned cc) {
        auto& f = s_.aflag;
        bool r = false;
        switch (cc >> 1) {
        case 0: r = f.of; break;
        case 1: r = f.cf; break;
        case 2: r = f.zf; break;
        case 3: r = f.cf || f.zf; break;
        case 4: r = f.sf; break;
        case 5: r = f.pf; break;
        case 6: r = f.sf != f.of; break;
        default: r = f.zf || (f.sf != f.of); break;
        }
        return (cc & 1) ? !r : r;
    }

    static int cc_of(ZydisMnemonic m) {
        switch (m) {
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return 0;
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return 1;
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return 2;
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return 3;
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return 4;
        case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return 5;
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return 6;
        case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return 7;
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return 8;
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return 9;
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: return 10;
        case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: return 11;
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return 12;
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return 13;
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return 14;
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return 15;
        default: return -1;
        }
    }

    uint32_t add(uint32_t a, uint32_t b, uint32_t carry, unsigned bits) {
        uint64_t wide = static_cast<uint64_t>(a & mask_of(bits)) + (b & mask_of(bits)) + carry;
        uint32_t r = static_cast<uint32_t>(wide) & mask_of(bits);
        s_.aflag.cf = (wide >> bits) & 1;
        s_.aflag.af = ((a ^ b ^ r) >> 4) & 1;
        s_.aflag.of = (((a ^ r) & (b ^ r)) & sign_bit(bits)) != 0;
        Flags::set_szp(s_, r, bits);
        return r;
    }

    uint32_t sub(uint32_t a, uint32_t b, uint32_t borrow, unsigned bits) {
        a &= mask_of(bits);
        b &= mask_of(bits);
        uint64_t wide = static_cast<uint64_t>(a) - b - borrow;
        uint32_t r = static_cast<uint32_t>(wide) & mask_of(bits);
        s_.aflag.cf = static_cast<uint64_t>(b) + borrow > a;
        s_.aflag.af = ((a ^ b ^ r) >> 4) & 1;
        s_.aflag.of = (((a ^ b) & (a ^ r)) & sign_bit(bits)) != 0;
        Flags::set_szp(s_, r, bits);
        return r;
    }

    uint32_t logic(uint32_t r, unsigned bits) {
        r &= mask_of(bits);
        s_.aflag.cf = 0;
        s_.aflag.of = 0;
        s_.aflag.af = 0;
        Flags::set_szp(s_, r, bits);
        return r;
    }

    Outcome execute_shift() {
        unsigned bits = width(0);
        uint32_t v = read(0) & mask_of(bits);
        uint32_t count = (insn_.operand_count_visible > 1 ? read(1) : 1) & 0x1f;
        if (count == 0) {
            return Outcome::next;
        }
        uint32_t r = 0;
        switch (insn_.mnemonic) {
        case ZYDIS_MNEMONIC_SHL:
            r = count >= bits ? 0 : (v << count) & mask_of(bits);
            s_.aflag.cf = count <= bits ? (v >> (bits - count)) & 1 : 0;
            s_.aflag.of = ((r >> (bits - 1)) & 1) != s_.aflag.cf;
            Flags::set_szp(s_, r, bits);
            break;
        case ZYDIS_MNEMONIC_SHR:
            r = count >= bits ? 0 : v >> count;
            s_.aflag.cf = (v >> (count - 1)) & 1;
            s_.aflag.of = (v >> (bits - 1)) & 1;
            Flags::set_szp(s_, r, bits);
            break;
        case ZYDIS_MNEMONIC_SAR: {
            int32_t sv = sign_extend(v, bits);
            r = static_cast<uint32_t>(sv >> std::min<uint32_t>(count, 31)) & mask_of(bits);
            s_.aflag.cf = (sv >> std::min<uint32_t>(count - 1, 31)) & 1;
            s_.aflag.of = 0;
            Flags::set_szp(s_, r, bits);
            break;
        }
        case ZYDIS_MNEMONIC_ROL: {
            unsigned c = count % bits;
            r = c == 0 ? v : ((v << c) | (v >> (bits - c))) & mask_of(bits);
            s_.aflag.cf = r & 1;
            s_.aflag.of = ((r >> (bits - 1)) & 1) ^ s_.aflag.cf;
            break;
        }
        case ZYDIS_MNEMONIC_ROR: {
            unsigned c = count % bits;
            r = c == 0 ? v : ((v >> c) | (v << (bits - c))) & mask_of(bits);
            s_.aflag.cf = (r >> (bits - 1)) & 1;
            s_.aflag.of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1;
            break;
        }
        case ZYDIS_MNEMONIC_RCL: {
            uint64_t wide = (static_cast<uint64_t>(s_.aflag.cf) << bits) | v;
            unsigned span = bits + 1;
            unsigned c = count % span;
            uint64_t rot = ((wide << c) | (wide >> (span - c))) & ((1ull << span) - 1);
            r = static_cast<uint32_t>(rot) & mask_of(bits);
            s_.aflag.cf = (rot >> bits) & 1;
            s_.aflag.of = ((r >> (bits - 1)) & 1) ^ s_.aflag.cf;
            break;
        }
        case ZYDIS_MNEMONIC_RCR: {
            uint64_t wide = (static_cast<uint64_t>(s_.aflag.cf) << bits) | v;
            unsigned span = bits + 1;
            unsigned c = count % span;
            uint64_t rot = ((wide >> c) | (wide << (span - c))) & ((1ull << span) - 1);
            r = static_cast<uint32_t>(rot) & mask_of(bits);
            s_.aflag.cf = (rot >> bits) & 1;
            s_.aflag.of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1;
            break;
        }
        default:
            return Outcome::unsupported;
        }
        write(0, r);
        return Outcome::next;
    }

    Outcome execute_double_shift() {
        unsigned bits = width(0);
        uint32_t dst = read(0);
        uint32_t src = read(1);
        uint32_t count = read(2) & 0x1f;
        if (count == 0) {
            return Outcome::next;
        }
        uint64_t r = 0;
        if (insn_.mnemonic == ZYDIS_MNEMONIC_SHLD) {
            uint64_t wide = (static_cast<uint64_t>(dst) << 32) | src;
            r = (wide << count) >> 32;
            s_.aflag.cf = (dst >> (bits - count)) & 1;
        } else {
            uint64_t wide = (static_cast<uint64_t>(src) << 32) | dst;
            r = wide >> count;
            s_.aflag.cf = (dst >> (count - 1)) & 1;
        }
        uint32_t result = static_cast<uint32_t>(r) & mask_of(bits);
        s_.aflag.of = ((result ^ dst) >> (bits - 1)) & 1;
        Flags::set_szp(s_, result, bits);
        write(0, result);
        return Outcome::next;
    }

    Outcome execute_string() {
        unsigned bits = insn_.operand_width;
        unsigned step = bits / 8;
        int32_t delta = s_.aflag.df ? -static_cast<int32_t>(step) : static_cast<int32_t>(step);
        bool rep = insn_.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
        bool repne = insn_.attributes & ZYDIS_ATTRIB_HAS_REPNE;
        auto& ecx = host::ecx(s_);
        auto& esi = host::esi(s_);
        auto& edi = host::edi(s_);
        uint32_t eax = host::eax(s_);
        for (;;) {
            if (rep && ecx == 0) {
                break;
            }
            bool compare = false;
            switch (insn_.mnemonic) {
            case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
                mem_write(edi, bits, mem_read(esi, bits));
                esi += delta;
                edi += delta;
                break;
            case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
                mem_write(edi, bits, eax);
                edi += delta;
                break;
            case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
                reg_write(bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX,
                          mem_read(esi, bits));
                esi += delta;
                break;
            case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
                sub(mem_read(esi, bits), mem_read(edi, bits), 0, bits);
                esi += delta;
                edi += delta;
                compare = true;
                break;
            case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
                sub(eax, mem_read(edi, bits), 0, bits);
                edi += delta;
                compare = true;
                break;
            default:
                return Outcome::unsupported;
            }
            if (!rep) {
                break;
            }
            --ecx;
            if (compare && (repne ? s_.aflag.zf : !s_.aflag.zf)) {
                break;
            }
        }
        return Outcome::next;
    }

    double st(unsigned i) {
        double v;
        std::memcpy(&v, reinterpret_cast<uint8_t*>(&s_.st.elems[i]) + 6, sizeof(v));
        return v;
    }

    void set_st(unsigned i, double v) { std::memcpy(reinterpret_cast<uint8_t*>(&s_.st.elems[i]) + 6, &v, sizeof(v)); }

    void fpush(double v) {
        for (int i = 7; i > 0; --i) {
            std::memcpy(&s_.st.elems[i], &s_.st.elems[i - 1], sizeof(s_.st.elems[i]));
        }
        set_st(0, v);
        s_.x87.fxsave32.swd.top = static_cast<uint16_t>((s_.x87.fxsave32.swd.top + 7) % 8);
    }

    double fpop() {
        double v = st(0);
        auto saved = s_.st.elems[0];
        for (int i = 0; i < 7; ++i) {
            std::memcpy(&s_.st.elems[i], &s_.st.elems[i + 1], sizeof(s_.st.elems[i]));
        }
        std::memcpy(&s_.st.elems[7], &saved, sizeof(saved));
        s_.x87.fxsave32.swd.top = static_cast<uint16_t>((s_.x87.fxsave32.swd.top + 9) % 8);
        return v;
    }

    unsigned st_index(ZydisRegister r) const { return static_cast<unsigned>(r - ZYDIS_REGISTER_ST0); }

    bool is_st(unsigned i) const {
        return ops_[i].type == ZYDIS_OPERAND_TYPE_REGISTER && ops_[i].reg.value >= ZYDIS_REGISTER_ST0 &&
               ops_[i].reg.value <= ZYDIS_REGISTER_ST7;
    }

    double fread_mem(unsigned i, bool integer) {
        uint32_t a = address(ops_[i]);
        if (integer) {
            switch (ops_[i].size) {
            case 16: return static_cast<double>(host::mem::read<int16_t>(a));
            case 32: return static_cast<double>(host::mem::read<int32_t>(a));
            default: return static_cast<double>(host::mem::read<int64_t>(a));
            }
        }
        switch (ops_[i].size) {
        case 32: return static_cast<double>(host::mem::read<float>(a));
        case 64: return host::mem::read<double>(a);
        default: return read_f80(a);
        }
    }

    double read_f80(uint32_t a) {
        uint64_t mantissa = host::mem::read<uint64_t>(a);
        uint16_t se = host::mem::read<uint16_t>(a + 8);
        bool negative = se & 0x8000;
        int exponent = se & 0x7fff;
        double v;
        if (exponent == 0 && mantissa == 0) {
            v = 0.0;
        } else if (exponent == 0x7fff) {
            v = (mantissa << 1) == 0 ? std::numeric_limits<double>::infinity()
                                     : std::numeric_limits<double>::quiet_NaN();
        } else {
            v = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
        }
        return negative ? -v : v;
    }

    void write_f80(uint32_t a, double v) {
        uint64_t mantissa = 0;
        uint16_t se = std::signbit(v) ? 0x8000 : 0;
        double m = std::fabs(v);
        if (std::isnan(v)) {
            se |= 0x7fff;
            mantissa = 0xc000000000000000ull;
        } else if (std::isinf(v)) {
            se |= 0x7fff;
            mantissa = 0x8000000000000000ull;
        } else if (m != 0.0) {
            int e = 0;
            double frac = std::frexp(m, &e);
            mantissa = static_cast<uint64_t>(std::ldexp(frac, 64));
            se |= static_cast<uint16_t>(e - 1 + 16383);
        }
        host::mem::write<uint64_t>(a, mantissa);
        host::mem::write<uint16_t>(a + 8, se);
    }

    void fwrite_mem(unsigned i, double v, bool integer) {
        uint32_t a = address(ops_[i]);
        if (integer) {
            double r = std::nearbyint(v);
            switch (ops_[i].size) {
            case 16: {
                bool ok = r >= -32768.0 && r <= 32767.0;
                host::mem::write<int16_t>(a, ok ? static_cast<int16_t>(r) : int16_t{-32768});
                break;
            }
            case 32: {
                bool ok = r >= -2147483648.0 && r <= 2147483647.0;
                host::mem::write<int32_t>(a, ok ? static_cast<int32_t>(r) : std::numeric_limits<int32_t>::min());
                break;
            }
            default: {
                bool ok = r >= -9223372036854775808.0 && r < 9223372036854775808.0;
                host::mem::write<int64_t>(a, ok ? static_cast<int64_t>(r) : std::numeric_limits<int64_t>::min());
                break;
            }
            }
            return;
        }
        switch (ops_[i].size) {
        case 32: host::mem::write<float>(a, static_cast<float>(v)); break;
        case 64: host::mem::write<double>(a, v); break;
        default: write_f80(a, v); break;
        }
    }

    void fcompare(double a, double b, bool to_eflags) {
        bool unordered = std::isnan(a) || std::isnan(b);
        bool less = !unordered && a < b;
        bool equal = !unordered && a == b;
        if (to_eflags) {
            s_.aflag.zf = unordered || equal;
            s_.aflag.pf = unordered;
            s_.aflag.cf = unordered || less;
            s_.aflag.of = 0;
            s_.aflag.sf = 0;
            s_.aflag.af = 0;
        } else {
            s_.sw.c3 = unordered || equal;
            s_.sw.c2 = unordered;
            s_.sw.c0 = unordered || less;
            s_.sw.c1 = 0;
        }
    }

    uint16_t status_word() {
        auto& sw = s_.x87.fxsave32.swd;
        sw.c0 = s_.sw.c0 & 1;
        sw.c1 = s_.sw.c1 & 1;
        sw.c2 = s_.sw.c2 & 1;
        sw.c3 = s_.sw.c3 & 1;
        sw.pe = s_.sw.pe & 1;
        sw.ue = s_.sw.ue & 1;
        sw.oe = s_.sw.oe & 1;
        sw.ze = s_.sw.ze & 1;
        sw.de = s_.sw.de & 1;
        sw.ie = s_.sw.ie & 1;
        return sw.flat;
    }

    void set_rounding(uint16_t cw) {
        static constexpr int kModes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
        std::fesetround(kModes[(cw >> 10) & 3]);
    }

    Outcome execute_fpu_arith(ZydisMnemonic m) {
        bool integer = m == ZYDIS_MNEMONIC_FIADD || m == ZYDIS_MNEMONIC_FISUB || m == ZYDIS_MNEMONIC_FISUBR ||
                       m == ZYDIS_MNEMONIC_FIMUL || m == ZYDIS_MNEMONIC_FIDIV || m == ZYDIS_MNEMONIC_FIDIVR;
        bool pop = m == ZYDIS_MNEMONIC_FADDP || m == ZYDIS_MNEMONIC_FSUBP || m == ZYDIS_MNEMONIC_FSUBRP ||
                   m == ZYDIS_MNEMONIC_FMULP || m == ZYDIS_MNEMONIC_FDIVP || m == ZYDIS_MNEMONIC_FDIVRP;
        unsigned dst = 0;
        double a = 0;
        double b = 0;
        unsigned visible = insn_.operand_count_visible;
        if (visible >= 1 && ops_[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            a = st(0);
            b = fread_mem(0, integer);
        } else if (visible >= 2 && is_st(0) && is_st(1)) {
            dst = st_index(ops_[0].reg.value);
            a = st(dst);
            b = st(st_index(ops_[1].reg.value));
        } else if (visible >= 2 && is_st(0) && ops_[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            a = st(0);
            b = fread_mem(1, integer);
        } else {
            return Outcome::unsupported;
        }
        double r = 0;
        switch (m) {
        case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD: r = a + b; break;
        case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB: r = a - b; break;
        case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR: r = b - a; break;
        case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL: r = a * b; break;
        case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV: r = a / b; break;
        case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR: r = b / a; break;
        default: return Outcome::unsupported;
        }
        set_st(dst, r);
        if (pop) {
            fpop();
        }
        return Outcome::next;
    }

    Outcome execute_fpu() {
        auto m = insn_.mnemonic;
        unsigned visible = insn_.operand_count_visible;
        switch (m) {
        case ZYDIS_MNEMONIC_FLD:
            if (is_st(0)) {
                fpush(st(st_index(ops_[0].reg.value)));
            } else {
                fpush(fread_mem(0, false));
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_FILD:
            fpush(fread_mem(0, true));
            return Outcome::next;
        case ZYDIS_MNEMONIC_FST:
        case ZYDIS_MNEMONIC_FSTP:
            if (is_st(0)) {
                set_st(st_index(ops_[0].reg.value), st(0));
            } else {
                fwrite_mem(0, st(0), false);
            }
            if (m == ZYDIS_MNEMONIC_FSTP) {
                fpop();
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_FIST:
        case ZYDIS_MNEMONIC_FISTP:
            fwrite_mem(0, st(0), true);
            if (m == ZYDIS_MNEMONIC_FISTP) {
                fpop();
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_FISTTP: {
            double saved = st(0);
            int mode = std::fegetround();
            std::fesetround(FE_TOWARDZERO);
            fwrite_mem(0, saved, true);
            std::fesetround(mode);
            fpop();
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FLDZ: fpush(0.0); return Outcome::next;
        case ZYDIS_MNEMONIC_FLD1: fpush(1.0); return Outcome::next;
        case ZYDIS_MNEMONIC_FLDPI: fpush(3.14159265358979323846); return Outcome::next;
        case ZYDIS_MNEMONIC_FLDL2E: fpush(1.44269504088896340736); return Outcome::next;
        case ZYDIS_MNEMONIC_FLDL2T: fpush(3.32192809488736234787); return Outcome::next;
        case ZYDIS_MNEMONIC_FLDLG2: fpush(0.30102999566398119521); return Outcome::next;
        case ZYDIS_MNEMONIC_FLDLN2: fpush(0.69314718055994530942); return Outcome::next;
        case ZYDIS_MNEMONIC_FXCH: {
            unsigned i = visible >= 2 && is_st(1) ? st_index(ops_[1].reg.value) : 1;
            double t = st(0);
            set_st(0, st(i));
            set_st(i, t);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD:
        case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB:
        case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR:
        case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL:
        case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
        case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR:
            return execute_fpu_arith(m);
        case ZYDIS_MNEMONIC_FCHS: set_st(0, -st(0)); return Outcome::next;
        case ZYDIS_MNEMONIC_FABS: set_st(0, std::fabs(st(0))); return Outcome::next;
        case ZYDIS_MNEMONIC_FSQRT: set_st(0, std::sqrt(st(0))); return Outcome::next;
        case ZYDIS_MNEMONIC_FSIN: set_st(0, std::sin(st(0))); s_.sw.c2 = 0; return Outcome::next;
        case ZYDIS_MNEMONIC_FCOS: set_st(0, std::cos(st(0))); s_.sw.c2 = 0; return Outcome::next;
        case ZYDIS_MNEMONIC_FSINCOS: {
            double v = st(0);
            set_st(0, std::sin(v));
            fpush(std::cos(v));
            s_.sw.c2 = 0;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FPTAN:
            set_st(0, std::tan(st(0)));
            fpush(1.0);
            s_.sw.c2 = 0;
            return Outcome::next;
        case ZYDIS_MNEMONIC_FPATAN: {
            double r = std::atan2(st(1), st(0));
            fpop();
            set_st(0, r);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FRNDINT: set_st(0, std::nearbyint(st(0))); return Outcome::next;
        case ZYDIS_MNEMONIC_FSCALE: set_st(0, std::ldexp(st(0), static_cast<int>(std::trunc(st(1))))); return Outcome::next;
        case ZYDIS_MNEMONIC_FPREM: case ZYDIS_MNEMONIC_FPREM1: {
            double r = m == ZYDIS_MNEMONIC_FPREM ? std::fmod(st(0), st(1)) : std::remainder(st(0), st(1));
            set_st(0, r);
            s_.sw.c2 = 0;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP: {
            double b = 0;
            if (visible >= 1 && ops_[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                b = fread_mem(0, false);
            } else if (visible >= 2 && is_st(1)) {
                b = st(st_index(ops_[1].reg.value));
            } else if (visible >= 1 && is_st(0) && ops_[0].reg.value != ZYDIS_REGISTER_ST0) {
                b = st(st_index(ops_[0].reg.value));
            } else {
                b = st(1);
            }
            fcompare(st(0), b, false);
            if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP) {
                fpop();
            }
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP:
            fcompare(st(0), fread_mem(0, true), false);
            if (m == ZYDIS_MNEMONIC_FICOMP) {
                fpop();
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP:
            fcompare(st(0), st(1), false);
            fpop();
            fpop();
            return Outcome::next;
        case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
            unsigned i = visible >= 2 && is_st(1) ? st_index(ops_[1].reg.value) : 1;
            fcompare(st(0), st(i), true);
            if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) {
                fpop();
            }
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FTST:
            fcompare(st(0), 0.0, false);
            return Outcome::next;
        case ZYDIS_MNEMONIC_FXAM: {
            double v = st(0);
            s_.sw.c1 = std::signbit(v);
            int cls = std::fpclassify(v);
            s_.sw.c3 = cls == FP_ZERO || cls == FP_SUBNORMAL;
            s_.sw.c2 = cls == FP_NORMAL || cls == FP_INFINITE || cls == FP_SUBNORMAL;
            s_.sw.c0 = cls == FP_NAN || cls == FP_INFINITE;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FNSTSW:
            if (ops_[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                reg_write(ZYDIS_REGISTER_AX, status_word());
            } else {
                host::mem::write<uint16_t>(address(ops_[0]), status_word());
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_FNSTCW:
            host::mem::write<uint16_t>(address(ops_[0]), s_.x87.fxsave32.cwd.flat);
            return Outcome::next;
        case ZYDIS_MNEMONIC_FLDCW: {
            uint16_t cw = host::mem::read<uint16_t>(address(ops_[0]));
            s_.x87.fxsave32.cwd.flat = cw;
            set_rounding(cw);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_FNCLEX:
            s_.sw.ie = s_.sw.de = s_.sw.ze = s_.sw.oe = s_.sw.ue = s_.sw.pe = 0;
            return Outcome::next;
        case ZYDIS_MNEMONIC_FWAIT:
        case ZYDIS_MNEMONIC_FNOP:
        case ZYDIS_MNEMONIC_FFREE:
            return Outcome::next;
        case ZYDIS_MNEMONIC_FINCSTP:
            s_.x87.fxsave32.swd.top = static_cast<uint16_t>((s_.x87.fxsave32.swd.top + 1) % 8);
            return Outcome::next;
        case ZYDIS_MNEMONIC_FDECSTP:
            s_.x87.fxsave32.swd.top = static_cast<uint16_t>((s_.x87.fxsave32.swd.top + 7) % 8);
            return Outcome::next;
        default:
            return Outcome::unsupported;
        }
    }

    Outcome call_to(uint32_t target) {
        push(next_);
        host::eip(s_) = target;
        hooks_.call_native(hooks_.context, s_, target);
        next_ = host::eip(s_);
        return Outcome::next;
    }

    Outcome execute() {
        bad_ = false;
        Outcome o = execute_inner();
        return bad_ ? Outcome::unsupported : o;
    }

    Outcome execute_inner() {
        auto m = insn_.mnemonic;
        if (int cc = cc_of(m); cc >= 0) {
            if (m >= ZYDIS_MNEMONIC_JB && m <= ZYDIS_MNEMONIC_JZ) {
                if (condition(static_cast<unsigned>(cc))) {
                    next_ = branch_target(0);
                }
            } else if (m >= ZYDIS_MNEMONIC_SETB && m <= ZYDIS_MNEMONIC_SETZ) {
                write(0, condition(static_cast<unsigned>(cc)) ? 1 : 0);
            } else {
                uint32_t v = read(1);
                if (condition(static_cast<unsigned>(cc))) {
                    write(0, v);
                }
            }
            return Outcome::next;
        }
        if (insn_.meta.category == ZYDIS_CATEGORY_X87_ALU || insn_.meta.isa_set == ZYDIS_ISA_SET_X87) {
            return execute_fpu();
        }
        switch (m) {
        case ZYDIS_MNEMONIC_NOP:
        case ZYDIS_MNEMONIC_PAUSE:
            return Outcome::next;
        case ZYDIS_MNEMONIC_MOV:
            write(0, read(1));
            return Outcome::next;
        case ZYDIS_MNEMONIC_MOVZX:
            write(0, read(1));
            return Outcome::next;
        case ZYDIS_MNEMONIC_MOVSX:
            write(0, static_cast<uint32_t>(sign_extend(read(1), width(1))));
            return Outcome::next;
        case ZYDIS_MNEMONIC_LEA:
            write(0, address(ops_[1]));
            return Outcome::next;
        case ZYDIS_MNEMONIC_PUSH:
            push(read_as(0, 32));
            return Outcome::next;
        case ZYDIS_MNEMONIC_POP: {
            uint32_t v = pop();
            write(0, v);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_PUSHAD: {
            uint32_t original = host::esp(s_);
            push(host::eax(s_));
            push(host::ecx(s_));
            push(host::edx(s_));
            push(host::ebx(s_));
            push(original);
            push(host::ebp(s_));
            push(host::esi(s_));
            push(host::edi(s_));
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_POPAD:
            host::edi(s_) = pop();
            host::esi(s_) = pop();
            host::ebp(s_) = pop();
            pop();
            host::ebx(s_) = pop();
            host::edx(s_) = pop();
            host::ecx(s_) = pop();
            host::eax(s_) = pop();
            return Outcome::next;
        case ZYDIS_MNEMONIC_PUSHFD:
            push(eflags());
            return Outcome::next;
        case ZYDIS_MNEMONIC_POPFD:
            set_eflags(pop());
            return Outcome::next;
        case ZYDIS_MNEMONIC_LAHF:
            reg_write(ZYDIS_REGISTER_AH, eflags() & 0xff);
            return Outcome::next;
        case ZYDIS_MNEMONIC_SAHF: {
            uint32_t ah = reg_read(ZYDIS_REGISTER_AH);
            set_eflags((eflags() & ~0xffu) | ah);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_ADD:
            write(0, add(read(0), read_as(1, width(0)), 0, width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_ADC:
            write(0, add(read(0), read_as(1, width(0)), s_.aflag.cf, width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_SUB:
            write(0, sub(read(0), read_as(1, width(0)), 0, width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_SBB:
            write(0, sub(read(0), read_as(1, width(0)), s_.aflag.cf, width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_CMP:
            sub(read(0), read_as(1, width(0)), 0, width(0));
            return Outcome::next;
        case ZYDIS_MNEMONIC_AND:
            write(0, logic(read(0) & read_as(1, width(0)), width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_OR:
            write(0, logic(read(0) | read_as(1, width(0)), width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_XOR:
            write(0, logic(read(0) ^ read_as(1, width(0)), width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_TEST:
            logic(read(0) & read_as(1, width(0)), width(0));
            return Outcome::next;
        case ZYDIS_MNEMONIC_INC: {
            uint8_t cf = s_.aflag.cf;
            write(0, add(read(0), 1, 0, width(0)));
            s_.aflag.cf = cf;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_DEC: {
            uint8_t cf = s_.aflag.cf;
            write(0, sub(read(0), 1, 0, width(0)));
            s_.aflag.cf = cf;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_NEG: {
            uint32_t v = read(0);
            write(0, sub(0, v, 0, width(0)));
            s_.aflag.cf = (v & mask_of(width(0))) != 0;
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_NOT:
            write(0, ~read(0) & mask_of(width(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
        case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR:
            return execute_shift();
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD:
            return execute_double_shift();
        case ZYDIS_MNEMONIC_IMUL:
            return execute_imul();
        case ZYDIS_MNEMONIC_MUL:
            return execute_mul();
        case ZYDIS_MNEMONIC_DIV:
        case ZYDIS_MNEMONIC_IDIV:
            return execute_div(m == ZYDIS_MNEMONIC_IDIV);
        case ZYDIS_MNEMONIC_CDQ:
            host::edx(s_) = static_cast<int32_t>(host::eax(s_)) < 0 ? 0xffffffffu : 0;
            return Outcome::next;
        case ZYDIS_MNEMONIC_CWD:
            reg_write(ZYDIS_REGISTER_DX, (host::eax(s_) & 0x8000) ? 0xffff : 0);
            return Outcome::next;
        case ZYDIS_MNEMONIC_CWDE:
            host::eax(s_) = static_cast<uint32_t>(sign_extend(host::eax(s_), 16));
            return Outcome::next;
        case ZYDIS_MNEMONIC_CBW:
            reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(sign_extend(host::eax(s_), 8)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_XCHG: {
            uint32_t a = read(0);
            uint32_t b = read(1);
            write(0, b);
            write(1, a);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_BSWAP:
            write(0, __builtin_bswap32(read(0)));
            return Outcome::next;
        case ZYDIS_MNEMONIC_XADD: {
            uint32_t a = read(0);
            uint32_t b = read(1);
            uint32_t sum = add(a, b, 0, width(0));
            write(1, a);
            write(0, sum);
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_CMPXCHG: {
            unsigned bits = width(0);
            ZydisRegister acc = bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            uint32_t current = read(0);
            sub(reg_read(acc), current, 0, bits);
            if (s_.aflag.zf) {
                write(0, read(1));
            } else {
                reg_write(acc, current);
            }
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: {
            unsigned bits = width(0);
            uint32_t bit = read(1);
            if (ops_[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops_[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                int32_t offset = static_cast<int32_t>(bit) >> 3;
                uint32_t a = address(ops_[0]) + static_cast<uint32_t>(offset);
                uint8_t byte = host::mem::read<uint8_t>(a);
                unsigned b = bit & 7;
                s_.aflag.cf = (byte >> b) & 1;
                if (m == ZYDIS_MNEMONIC_BTS) byte |= static_cast<uint8_t>(1u << b);
                if (m == ZYDIS_MNEMONIC_BTR) byte &= static_cast<uint8_t>(~(1u << b));
                if (m == ZYDIS_MNEMONIC_BTC) byte ^= static_cast<uint8_t>(1u << b);
                if (m != ZYDIS_MNEMONIC_BT) host::mem::write<uint8_t>(a, byte);
                return Outcome::next;
            }
            bit %= bits;
            uint32_t v = read(0);
            s_.aflag.cf = (v >> bit) & 1;
            if (m == ZYDIS_MNEMONIC_BTS) write(0, v | (1u << bit));
            if (m == ZYDIS_MNEMONIC_BTR) write(0, v & ~(1u << bit));
            if (m == ZYDIS_MNEMONIC_BTC) write(0, v ^ (1u << bit));
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_BSF:
        case ZYDIS_MNEMONIC_BSR: {
            uint32_t v = read(1) & mask_of(width(1));
            s_.aflag.zf = v == 0;
            if (v != 0) {
                write(0, m == ZYDIS_MNEMONIC_BSF ? std::countr_zero(v) : 31 - std::countl_zero(v));
            }
            return Outcome::next;
        }
        case ZYDIS_MNEMONIC_CLD: s_.aflag.df = 0; return Outcome::next;
        case ZYDIS_MNEMONIC_STD: s_.aflag.df = 1; return Outcome::next;
        case ZYDIS_MNEMONIC_CLC: s_.aflag.cf = 0; return Outcome::next;
        case ZYDIS_MNEMONIC_STC: s_.aflag.cf = 1; return Outcome::next;
        case ZYDIS_MNEMONIC_CMC: s_.aflag.cf = !s_.aflag.cf; return Outcome::next;
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW:
        case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW:
        case ZYDIS_MNEMONIC_SCASD:
            return execute_string();
        case ZYDIS_MNEMONIC_MOVSD:
        case ZYDIS_MNEMONIC_CMPSD:
            if (insn_.meta.isa_set == ZYDIS_ISA_SET_I386 || insn_.meta.category == ZYDIS_CATEGORY_STRINGOP) {
                return execute_string();
            }
            return Outcome::unsupported;
        case ZYDIS_MNEMONIC_LEAVE:
            host::esp(s_) = host::ebp(s_);
            host::ebp(s_) = pop();
            return Outcome::next;
        case ZYDIS_MNEMONIC_JMP:
            next_ = branch_target(0);
            return Outcome::next;
        case ZYDIS_MNEMONIC_JECXZ:
            if (host::ecx(s_) == 0) {
                next_ = branch_target(0);
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_LOOP:
            if (--host::ecx(s_) != 0) {
                next_ = branch_target(0);
            }
            return Outcome::next;
        case ZYDIS_MNEMONIC_CALL:
            return call_to(branch_target(0));
        case ZYDIS_MNEMONIC_RET: {
            uint32_t target = pop();
            if (insn_.operand_count_visible > 0) {
                host::esp(s_) += read(0);
            }
            host::eip(s_) = target;
            return Outcome::returned;
        }
        default:
            return Outcome::unsupported;
        }
    }

    Outcome execute_imul() {
        unsigned visible = insn_.operand_count_visible;
        if (visible == 1) {
            unsigned bits = width(0);
            int64_t a = sign_extend(reg_read(bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX
                                                                                       : ZYDIS_REGISTER_EAX),
                                    bits);
            int64_t b = sign_extend(read(0), bits);
            int64_t r = a * b;
            if (bits == 8) {
                reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(r));
            } else if (bits == 16) {
                reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(r));
                reg_write(ZYDIS_REGISTER_DX, static_cast<uint32_t>(r >> 16));
            } else {
                host::eax(s_) = static_cast<uint32_t>(r);
                host::edx(s_) = static_cast<uint32_t>(r >> 32);
            }
            bool overflow = r != sign_extend(static_cast<uint32_t>(r), bits);
            s_.aflag.cf = s_.aflag.of = overflow;
            return Outcome::next;
        }
        unsigned bits = width(0);
        int64_t a = sign_extend(visible == 3 ? read(1) : read(0), bits);
        int64_t b = visible == 3 ? sign_extend(read_as(2, bits), bits) : sign_extend(read_as(1, bits), bits);
        int64_t r = a * b;
        uint32_t truncated = static_cast<uint32_t>(r) & mask_of(bits);
        write(0, truncated);
        bool overflow = r != sign_extend(truncated, bits);
        s_.aflag.cf = s_.aflag.of = overflow;
        Flags::set_szp(s_, truncated, bits);
        return Outcome::next;
    }

    Outcome execute_mul() {
        unsigned bits = width(0);
        uint64_t b = read(0) & mask_of(bits);
        if (bits == 8) {
            uint64_t r = (host::eax(s_) & 0xff) * b;
            reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(r));
            s_.aflag.cf = s_.aflag.of = (r >> 8) != 0;
        } else if (bits == 16) {
            uint64_t r = (host::eax(s_) & 0xffff) * b;
            reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(r));
            reg_write(ZYDIS_REGISTER_DX, static_cast<uint32_t>(r >> 16));
            s_.aflag.cf = s_.aflag.of = (r >> 16) != 0;
        } else {
            uint64_t r = static_cast<uint64_t>(host::eax(s_)) * b;
            host::eax(s_) = static_cast<uint32_t>(r);
            host::edx(s_) = static_cast<uint32_t>(r >> 32);
            s_.aflag.cf = s_.aflag.of = (r >> 32) != 0;
        }
        return Outcome::next;
    }

    Outcome execute_div(bool is_signed) {
        unsigned bits = width(0);
        uint32_t divisor = read(0) & mask_of(bits);
        if (divisor == 0) {
            return Outcome::unsupported;
        }
        if (bits == 32) {
            uint64_t dividend = (static_cast<uint64_t>(host::edx(s_)) << 32) | host::eax(s_);
            if (is_signed) {
                auto n = static_cast<int64_t>(dividend);
                auto d = static_cast<int64_t>(static_cast<int32_t>(divisor));
                int64_t q = n / d;
                if (q > std::numeric_limits<int32_t>::max() || q < std::numeric_limits<int32_t>::min()) {
                    return Outcome::unsupported;
                }
                host::eax(s_) = static_cast<uint32_t>(q);
                host::edx(s_) = static_cast<uint32_t>(n % d);
            } else {
                uint64_t q = dividend / divisor;
                if (q > 0xffffffffull) {
                    return Outcome::unsupported;
                }
                host::eax(s_) = static_cast<uint32_t>(q);
                host::edx(s_) = static_cast<uint32_t>(dividend % divisor);
            }
            return Outcome::next;
        }
        if (bits == 16) {
            uint32_t dividend = ((host::edx(s_) & 0xffff) << 16) | (host::eax(s_) & 0xffff);
            if (is_signed) {
                auto n = static_cast<int32_t>(dividend);
                int32_t d = sign_extend(divisor, 16);
                int32_t q = n / d;
                if (q > 32767 || q < -32768) {
                    return Outcome::unsupported;
                }
                reg_write(ZYDIS_REGISTER_AX, static_cast<uint32_t>(q));
                reg_write(ZYDIS_REGISTER_DX, static_cast<uint32_t>(n % d));
            } else {
                uint32_t q = dividend / divisor;
                if (q > 0xffff) {
                    return Outcome::unsupported;
                }
                reg_write(ZYDIS_REGISTER_AX, q);
                reg_write(ZYDIS_REGISTER_DX, dividend % divisor);
            }
            return Outcome::next;
        }
        uint32_t dividend = host::eax(s_) & 0xffff;
        if (is_signed) {
            int32_t n = sign_extend(dividend, 16);
            int32_t d = sign_extend(divisor, 8);
            int32_t q = n / d;
            if (q > 127 || q < -128) {
                return Outcome::unsupported;
            }
            reg_write(ZYDIS_REGISTER_AL, static_cast<uint32_t>(q));
            reg_write(ZYDIS_REGISTER_AH, static_cast<uint32_t>(n % d));
        } else {
            uint32_t q = dividend / divisor;
            if (q > 0xff) {
                return Outcome::unsupported;
            }
            reg_write(ZYDIS_REGISTER_AL, q);
            reg_write(ZYDIS_REGISTER_AH, dividend % divisor);
        }
        return Outcome::next;
    }

    uint32_t eflags() {
        auto& f = s_.aflag;
        return 0x2u | (f.cf & 1u) | ((f.pf & 1u) << 2) | ((f.af & 1u) << 4) | ((f.zf & 1u) << 6) | ((f.sf & 1u) << 7) |
               ((f.df & 1u) << 10) | ((f.of & 1u) << 11) | 0x200u;
    }

    void set_eflags(uint32_t v) {
        auto& f = s_.aflag;
        f.cf = v & 1;
        f.pf = (v >> 2) & 1;
        f.af = (v >> 4) & 1;
        f.zf = (v >> 6) & 1;
        f.sf = (v >> 7) & 1;
        f.df = (v >> 10) & 1;
        f.of = (v >> 11) & 1;
    }

    GuestState& s_;
    const Hooks& hooks_;
    ZydisDecoder decoder_{};
    ZydisDecodedInstruction insn_{};
    ZydisDecodedOperand ops_[ZYDIS_MAX_OPERAND_COUNT]{};
    uint32_t next_ = 0;
    bool bad_ = false;
};

}

Result run(host::GuestState& s, uint32_t pc, const Hooks& hooks) {
    Machine machine(s, hooks);
    return machine.run(pc);
}

std::string describe(uint32_t pc) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    const uint8_t* bytes = host::mem::ptr(pc);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, 15, &insn, ops))) {
        return std::format("{:02x} {:02x} {:02x} {:02x} (undecodable)", bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    char text[256];
    ZydisFormatterFormatInstruction(&formatter, &insn, ops, insn.operand_count_visible, text, sizeof(text), pc,
                                    nullptr);
    std::string hex;
    for (unsigned i = 0; i < insn.length; ++i) {
        hex += std::format("{:02x}", bytes[i]);
    }
    return std::format("{} [{}]", text, hex);
}

}
