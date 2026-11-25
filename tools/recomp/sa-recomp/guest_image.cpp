#include "guest_image.hpp"

#include "macandreas/exe_identity.hpp"

#include <algorithm>
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
