#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ma::pe {

inline constexpr uint16_t kMachineI386 = 0x014c;
inline constexpr uint16_t kOptionalMagicPe32 = 0x010b;

inline constexpr uint32_t kSectionExecute = 0x20000000;
inline constexpr uint32_t kSectionRead = 0x40000000;
inline constexpr uint32_t kSectionWrite = 0x80000000;

enum class Directory : uint32_t {
    exports = 0,
    imports = 1,
    resources = 2,
    exceptions = 3,
    security = 4,
    relocations = 5,
    debug = 6,
    tls = 9,
    load_config = 10,
    iat = 12,
};

struct Section {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;

    bool executable() const { return (characteristics & kSectionExecute) != 0; }
    bool readable() const { return (characteristics & kSectionRead) != 0; }
    bool writable() const { return (characteristics & kSectionWrite) != 0; }
    uint32_t mapped_size() const { return virtual_size > raw_size ? virtual_size : raw_size; }
};

struct ImportEntry {
    std::string name;
    std::optional<uint16_t> ordinal;
    uint16_t hint = 0;
    uint32_t iat_rva = 0;
};

struct ImportModule {
    std::string dll;
    std::vector<ImportEntry> entries;
};

struct DataDirectory {
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct Image {
    uint16_t machine = 0;
    uint32_t timestamp = 0;
    uint16_t file_characteristics = 0;
    uint32_t image_base = 0;
    uint32_t entry_rva = 0;
    uint32_t size_of_image = 0;
    uint32_t size_of_headers = 0;
    uint32_t section_alignment = 0;
    uint32_t file_alignment = 0;
    uint16_t subsystem = 0;
    std::vector<DataDirectory> directories;
    std::vector<Section> sections;
    std::vector<ImportModule> imports;

    DataDirectory directory(Directory d) const;
    std::optional<uint32_t> rva_to_offset(uint32_t rva) const;
    const Section* section_for_rva(uint32_t rva) const;
    size_t import_count() const;
    uint32_t entry_va() const { return image_base + entry_rva; }
};

std::expected<Image, std::string> parse(std::span<const uint8_t> data);

std::expected<std::vector<uint8_t>, std::string> read_file(const std::filesystem::path& path);

std::optional<std::string_view> known_ordinal_name(std::string_view dll, uint16_t ordinal);

}
