#pragma once

#include "macandreas/pe_image.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace ma::recomp {

class GuestImage {
public:
    static std::expected<GuestImage, std::string> load(const std::filesystem::path& exe);

    bool read_exec(uint32_t addr, uint8_t& out) const;
    bool is_exec(uint32_t addr) const;
    std::vector<uint32_t> data_pointers_into(uint32_t code_start, uint32_t code_end) const;
    std::vector<uint32_t> code_immediates_into(uint32_t code_start, uint32_t code_end) const;
    bool read_u32(uint32_t addr, uint32_t& out) const;
    bool patch(uint32_t addr, const std::vector<uint8_t>& bytes);
    bool is_call_at(uint32_t addr) const;
    std::vector<uint32_t> scan_jump_table(uint32_t site, uint32_t max_distance = 0x10000) const;
    uint32_t entry() const { return image_.entry_va(); }
    const pe::Image& image() const { return image_; }

private:
    struct Range {
        uint32_t start = 0;
        uint32_t end = 0;
        size_t file_offset = 0;
    };

    std::vector<uint8_t> bytes_;
    pe::Image image_;
    std::vector<Range> exec_;
};

}
