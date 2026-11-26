#include "guest_image.hpp"

#include "macandreas/exe_identity.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace ma::recomp {

std::expected<GuestImage, std::string> GuestImage::load(const std::filesystem::path& exe) {
    auto bytes = pe::read_file(exe);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto id = exe::identify(*bytes);
    if (id.support != exe::Support::supported) {
        return std::unexpected(id.message);
    }
    auto image = pe::parse(*bytes);
    if (!image) {
        return std::unexpected(image.error());
    }
    GuestImage g;
    g.bytes_ = std::move(*bytes);
    g.image_ = std::move(*image);
    for (const auto& s : g.image_.sections) {
        if (!s.executable() || s.raw_size == 0) {
            continue;
        }
        uint32_t start = g.image_.image_base + s.virtual_address;
        uint32_t backed = s.raw_size < s.mapped_size() ? s.raw_size : s.mapped_size();
        g.exec_.push_back({start, start + backed, s.raw_offset});
    }
    if (g.exec_.empty()) {
        return std::unexpected("no executable sections");
    }
    return g;
}

bool GuestImage::read_exec(uint32_t addr, uint8_t& out) const {
    for (const auto& r : exec_) {
        if (addr >= r.start && addr < r.end) {
            out = bytes_[r.file_offset + (addr - r.start)];
            return true;
        }
    }
    return false;
}

std::vector<uint32_t> GuestImage::data_pointers_into(uint32_t code_start, uint32_t code_end) const {
    std::vector<uint32_t> out;
    for (const auto& s : image_.sections) {
        if (s.executable() || s.raw_size < 4) {
            continue;
        }
        for (uint32_t off = 0; off + 4 <= s.raw_size; off += 4) {
            const uint8_t* p = bytes_.data() + s.raw_offset + off;
            uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            if (v >= code_start && v < code_end && is_exec(v)) {
                out.push_back(v);
            }
        }
    }
    return out;
}

std::vector<uint32_t> GuestImage::code_immediates_into(uint32_t code_start, uint32_t code_end) const {
    std::vector<uint32_t> out;
    for (const auto& r : exec_) {
        size_t length = r.end - r.start;
        const uint8_t* p = bytes_.data() + r.file_offset;
        auto value_at = [&](size_t at) {
            return static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) |
                   (static_cast<uint32_t>(p[at + 2]) << 16) | (static_cast<uint32_t>(p[at + 3]) << 24);
        };
        auto consider = [&](uint32_t v) {
            if (v >= code_start && v < code_end && is_exec(v)) {
                out.push_back(v);
            }
        };
        for (size_t i = 0; i + 5 <= length; ++i) {
            uint8_t op = p[i];
            if (op == 0x68 || (op >= 0xb8 && op <= 0xbf)) {
                consider(value_at(i + 1));
                continue;
            }
            if (op != 0xc7 || i + 2 > length) {
                continue;
            }
            uint8_t modrm = p[i + 1];
            if (((modrm >> 3) & 7) != 0) {
                continue;
            }
            unsigned mod = modrm >> 6;
            unsigned rm = modrm & 7;
            size_t operand = 0;
            if (mod != 3) {
                bool sib = rm == 4;
                size_t disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;
                if (mod == 0 && rm == 5) {
                    disp = 4;
                }
                if (mod == 0 && sib && i + 2 < length && (p[i + 2] & 7) == 5) {
                    disp = 4;
                }
                operand = (sib ? 1 : 0) + disp;
            }
            size_t imm = i + 2 + operand;
            if (imm + 4 <= length) {
                consider(value_at(imm));
            }
        }
    }
    return out;
}

bool GuestImage::read_u32(uint32_t addr, uint32_t& out) const {
    for (const auto& s : image_.sections) {
        uint32_t start = image_.image_base + s.virtual_address;
        if (addr < start || addr + 4 > start + s.raw_size) {
            continue;
        }
        const uint8_t* p = bytes_.data() + s.raw_offset + (addr - start);
        out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
              (static_cast<uint32_t>(p[3]) << 24);
        return true;
    }
    return false;
}

std::vector<uint32_t> GuestImage::scan_jump_table(uint32_t site, uint32_t max_distance) const {
    std::vector<uint32_t> out;
    uint8_t opcode = 0;
    uint8_t modrm = 0;
    uint8_t sib = 0;
    if (!read_exec(site, opcode) || !read_exec(site + 1, modrm) || !read_exec(site + 2, sib)) {
        return out;
    }
    bool scaled_index_no_base = (sib >> 6) == 2 && (sib & 7) == 5 && ((sib >> 3) & 7) != 4;
    if (opcode != 0xff || modrm != 0x24 || !scaled_index_no_base) {
        return out;
    }
    uint32_t table = 0;
    if (!read_u32(site + 3, table)) {
        return out;
    }
    uint32_t low = site > max_distance ? site - max_distance : 0;
    uint32_t high = site + max_distance;
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t target = 0;
        if (!read_u32(table + 4 * i, target) || target < low || target >= high || !is_exec(target)) {
            break;
        }
        out.push_back(target);
    }
    return out;
}

std::vector<uint32_t> GuestImage::computed_transfer_targets(uint32_t site) const {
    std::vector<uint32_t> out;
    uint32_t pc = site;
    auto byte = [&](uint8_t& v) { return read_exec(pc++, v); };
    auto dword = [&](uint32_t& v) {
        uint8_t b[4];
        for (auto& x : b) {
            if (!byte(x)) {
                return false;
            }
        }
        v = static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16 |
            static_cast<uint32_t>(b[3]) << 24;
        return true;
    };
    auto absolute = [&](uint8_t modrm, unsigned& reg, uint32_t& value) {
        uint32_t addr = 0;
        if ((modrm & 0xc7) != 0x05 || !dword(addr) || !read_u32(addr, value)) {
            return false;
        }
        reg = (modrm >> 3) & 7;
        return true;
    };
    uint8_t op = 0;
    if (!byte(op)) {
        return out;
    }
    bool call = false;
    if (op == 0xe8) {
        uint32_t rel = 0;
        if (!dword(rel) || rel != 0 || !byte(op) || op < 0x50 || op > 0x57) {
            return out;
        }
        call = true;
    } else if (op < 0x50 || op > 0x57) {
        return out;
    }
    unsigned pushed = op - 0x50u;
    std::array<uint32_t, 8> regs{};
    std::array<bool, 8> known{};
    std::array<uint32_t, 4> saved{};
    unsigned depth = 0;
    bool carry = false;
    bool flags_saved = false;
    for (int steps = 0; steps < 24; ++steps) {
        if (!byte(op)) {
            return out;
        }
        uint8_t modrm = 0;
        unsigned reg = 0;
        uint32_t value = 0;
        if (op == 0x9c && !flags_saved && !call) {
            flags_saved = true;
        } else if (op == 0x9d) {
            flags_saved = false;
        } else if (op == 0xf8 || op == 0xf9) {
            carry = op == 0xf9;
        } else if (op == 0xa1) {
            uint32_t addr = 0;
            if (!dword(addr) || !read_u32(addr, value)) {
                return out;
            }
            regs[0] = value;
            known[0] = true;
        } else if (op >= 0x50 && op <= 0x57) {
            if (depth == saved.size()) {
                return out;
            }
            saved[depth++] = regs[op - 0x50];
        } else if (op >= 0x58 && op <= 0x5f) {
            if (depth == 0) {
                return out;
            }
            regs[op - 0x58] = saved[--depth];
            known[op - 0x58] = false;
        } else if (op == 0x8b || op == 0x03 || op == 0x2b || op == 0x33 || op == 0x13 || op == 0x1b) {
            if (!byte(modrm) || !absolute(modrm, reg, value)) {
                return out;
            }
            if (op == 0x8b) {
                regs[reg] = value;
                known[reg] = true;
                continue;
            }
            if (!known[reg]) {
                return out;
            }
            uint64_t r = regs[reg];
            switch (op) {
            case 0x03: r += value; break;
            case 0x2b: r -= value; break;
            case 0x33: r ^= value; break;
            case 0x13: r += static_cast<uint64_t>(value) + (carry ? 1 : 0); break;
            default: r -= static_cast<uint64_t>(value) + (carry ? 1 : 0); break;
            }
            regs[reg] = static_cast<uint32_t>(r);
        } else if (op == 0xf7) {
            if (!byte(modrm) || (modrm & 0xf0) != 0xd0 || !known[modrm & 7]) {
                return out;
            }
            regs[modrm & 7] = (modrm & 8) ? 0u - regs[modrm & 7] : ~regs[modrm & 7];
        } else if (op == 0xd3) {
            if (!byte(modrm) || ((modrm & 0xf0) != 0xc0) || !known[modrm & 7] || !known[1]) {
                return out;
            }
            unsigned n = regs[1] & 31;
            uint32_t v = regs[modrm & 7];
            regs[modrm & 7] = n == 0 ? v : (modrm & 8) ? (v >> n | v << (32 - n)) : (v << n | v >> (32 - n));
        } else if (op == 0x87) {
            if (!byte(modrm) || (modrm & 0xc7) != 0x04) {
                return out;
            }
            uint8_t sib = 0;
            reg = (modrm >> 3) & 7;
            if (!byte(sib) || sib != 0x24 || reg != pushed || !known[reg] || depth != 0) {
                return out;
            }
            uint32_t target = regs[reg];
            if (call) {
                uint8_t kind = 0;
                uint8_t b1 = 0;
                uint8_t b2 = 0;
                uint8_t b3 = 0;
                uint32_t adjust = 0;
                if (!byte(kind) || !byte(b1) || !byte(b2) || !byte(b3) || b1 != 0x44 || b2 != 0x24 || b3 != 0x04) {
                    return out;
                }
                if (kind == 0x83) {
                    uint8_t imm = 0;
                    if (!byte(imm)) {
                        return out;
                    }
                    adjust = imm;
                } else if (kind != 0x81 || !dword(adjust)) {
                    return out;
                }
                out.push_back(site + 5 + adjust);
            } else if (flags_saved) {
                return out;
            }
            uint8_t ret = 0;
            if (!byte(ret) || ret != 0xc3) {
                out.clear();
                return out;
            }
            if (is_exec(target)) {
                out.push_back(target);
            }
            return out;
        } else {
            return out;
        }
    }
    return out;
}

bool GuestImage::patch(uint32_t addr, const std::vector<uint8_t>& bytes) {
    for (const auto& s : image_.sections) {
        uint32_t start = image_.image_base + s.virtual_address;
        if (addr < start || static_cast<uint64_t>(addr) + bytes.size() > static_cast<uint64_t>(start) + s.raw_size) {
            continue;
        }
        std::copy(bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(s.raw_offset + (addr - start)));
        return true;
    }
    return false;
}

bool GuestImage::is_call_at(uint32_t addr) const {
    uint8_t op = 0;
    uint8_t modrm = 0;
    if (!read_exec(addr, op)) {
        return false;
    }
    if (op == 0xe8) {
        return true;
    }
    return op == 0xff && read_exec(addr + 1, modrm) && ((modrm >> 3) & 7) == 2;
}

bool GuestImage::is_exec(uint32_t addr) const {
    uint8_t ignored;
    return read_exec(addr, ignored);
}

}
