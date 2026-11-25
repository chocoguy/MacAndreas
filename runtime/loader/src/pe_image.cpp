#include "macandreas/pe_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <utility>

namespace ma::pe {

namespace {

constexpr size_t kMaxImportDescriptors = 4096;
constexpr size_t kMaxImportEntries = 65536;
constexpr size_t kMaxStringLength = 4096;

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    bool has(size_t offset, size_t length) const {
        return offset <= data_.size() && length <= data_.size() - offset;
    }

    uint16_t u16(size_t offset) {
        if (!has(offset, 2)) {
            failed_ = true;
            return 0;
        }
        return static_cast<uint16_t>(data_[offset] | (data_[offset + 1] << 8));
    }

    uint32_t u32(size_t offset) {
        if (!has(offset, 4)) {
            failed_ = true;
            return 0;
        }
        return static_cast<uint32_t>(data_[offset]) | (static_cast<uint32_t>(data_[offset + 1]) << 8) |
               (static_cast<uint32_t>(data_[offset + 2]) << 16) | (static_cast<uint32_t>(data_[offset + 3]) << 24);
    }

    std::optional<std::string> cstring(size_t offset) const {
        if (offset >= data_.size()) {
            return std::nullopt;
        }
        size_t limit = std::min(data_.size(), offset + kMaxStringLength);
        for (size_t i = offset; i < limit; ++i) {
            if (data_[i] == 0) {
                return std::string(reinterpret_cast<const char*>(data_.data() + offset), i - offset);
            }
        }
        return std::nullopt;
    }

    std::string fixed_string(size_t offset, size_t length) {
        if (!has(offset, length)) {
            failed_ = true;
            return {};
        }
        const char* p = reinterpret_cast<const char*>(data_.data() + offset);
        return std::string(p, strnlen(p, length));
    }

    bool failed() const { return failed_; }

private:
    std::span<const uint8_t> data_;
    bool failed_ = false;
};

std::expected<std::vector<ImportModule>, std::string> parse_imports(const Image& image, Reader& r) {
    std::vector<ImportModule> modules;
    DataDirectory dir = image.directory(Directory::imports);
    if (dir.rva == 0) {
        return modules;
    }
    auto base = image.rva_to_offset(dir.rva);
    if (!base) {
        return std::unexpected(std::format("import directory rva 0x{:x} is not backed by file data", dir.rva));
    }
    for (size_t i = 0; i < kMaxImportDescriptors; ++i) {
        size_t d = *base + i * 20;
        uint32_t original_first_thunk = r.u32(d);
        uint32_t name_rva = r.u32(d + 12);
        uint32_t first_thunk = r.u32(d + 16);
        if (r.failed()) {
            return std::unexpected("import descriptor table is truncated");
        }
        if (name_rva == 0 && first_thunk == 0) {
            return modules;
        }
        auto name_offset = image.rva_to_offset(name_rva);
        auto name = name_offset ? r.cstring(*name_offset) : std::nullopt;
        if (!name) {
            return std::unexpected(std::format("import descriptor {} has an invalid name rva 0x{:x}", i, name_rva));
        }
        uint32_t lookup_rva = original_first_thunk != 0 ? original_first_thunk : first_thunk;
        auto lookup = image.rva_to_offset(lookup_rva);
        if (!lookup) {
            return std::unexpected(std::format("import lookup table for {} is not backed by file data", *name));
        }
        ImportModule module{.dll = *name, .entries = {}};
        for (size_t j = 0;; ++j) {
            if (j >= kMaxImportEntries) {
                return std::unexpected(std::format("import table for {} is unterminated", module.dll));
            }
            uint32_t thunk = r.u32(*lookup + j * 4);
            if (r.failed()) {
                return std::unexpected(std::format("import lookup table for {} is truncated", module.dll));
            }
            if (thunk == 0) {
                break;
            }
            ImportEntry entry;
            entry.iat_rva = first_thunk + static_cast<uint32_t>(j * 4);
            if (thunk & 0x80000000u) {
                entry.ordinal = static_cast<uint16_t>(thunk & 0xffff);
            } else {
                auto hint_offset = image.rva_to_offset(thunk);
                if (!hint_offset) {
                    return std::unexpected(std::format("import {}[{}] has an invalid hint/name rva 0x{:x}", module.dll, j, thunk));
                }
                entry.hint = r.u16(*hint_offset);
                auto entry_name = r.cstring(*hint_offset + 2);
                if (r.failed() || !entry_name) {
                    return std::unexpected(std::format("import {}[{}] has an unterminated name", module.dll, j));
                }
                entry.name = *entry_name;
            }
            module.entries.push_back(std::move(entry));
        }
        modules.push_back(std::move(module));
    }
    return std::unexpected("too many import descriptors");
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

struct OrdinalName {
    uint16_t ordinal;
    std::string_view name;
};

constexpr std::array kWs2_32Ordinals = {
    OrdinalName{1, "accept"},         OrdinalName{2, "bind"},
    OrdinalName{3, "closesocket"},    OrdinalName{4, "connect"},
    OrdinalName{5, "getpeername"},    OrdinalName{6, "getsockname"},
    OrdinalName{7, "getsockopt"},     OrdinalName{8, "htonl"},
    OrdinalName{9, "htons"},          OrdinalName{10, "ioctlsocket"},
    OrdinalName{11, "inet_addr"},     OrdinalName{12, "inet_ntoa"},
    OrdinalName{13, "listen"},        OrdinalName{14, "ntohl"},
    OrdinalName{15, "ntohs"},         OrdinalName{16, "recv"},
    OrdinalName{17, "recvfrom"},      OrdinalName{18, "select"},
    OrdinalName{19, "send"},          OrdinalName{20, "sendto"},
    OrdinalName{21, "setsockopt"},    OrdinalName{22, "shutdown"},
    OrdinalName{23, "socket"},        OrdinalName{51, "gethostbyaddr"},
    OrdinalName{52, "gethostbyname"}, OrdinalName{57, "gethostname"},
    OrdinalName{111, "WSAGetLastError"}, OrdinalName{112, "WSASetLastError"},
    OrdinalName{115, "WSAStartup"},   OrdinalName{116, "WSACleanup"},
    OrdinalName{151, "__WSAFDIsSet"},
};

}

DataDirectory Image::directory(Directory d) const {
    auto index = static_cast<size_t>(std::to_underlying(d));
    return index < directories.size() ? directories[index] : DataDirectory{};
}

std::optional<uint32_t> Image::rva_to_offset(uint32_t rva) const {
    if (rva < size_of_headers) {
        return rva;
    }
    for (const auto& s : sections) {
        if (rva >= s.virtual_address && rva - s.virtual_address < s.mapped_size()) {
            uint32_t delta = rva - s.virtual_address;
            if (delta >= s.raw_size) {
                return std::nullopt;
            }
            return s.raw_offset + delta;
        }
    }
    return std::nullopt;
}

const Section* Image::section_for_rva(uint32_t rva) const {
    for (const auto& s : sections) {
        if (rva >= s.virtual_address && rva - s.virtual_address < s.mapped_size()) {
            return &s;
        }
    }
    return nullptr;
}

size_t Image::import_count() const {
    size_t total = 0;
    for (const auto& m : imports) {
        total += m.entries.size();
    }
    return total;
}

std::expected<Image, std::string> parse(std::span<const uint8_t> data) {
    Reader r(data);
    if (!r.has(0, 0x40) || data[0] != 'M' || data[1] != 'Z') {
        return std::unexpected("not an MZ executable");
    }
    uint32_t pe = r.u32(0x3c);
    if (!r.has(pe, 24) || std::memcmp(data.data() + pe, "PE\0\0", 4) != 0) {
        return std::unexpected("missing PE signature");
    }

    Image image;
    image.machine = r.u16(pe + 4);
    uint16_t section_count = r.u16(pe + 6);
    image.timestamp = r.u32(pe + 8);
    uint16_t optional_size = r.u16(pe + 20);
    image.file_characteristics = r.u16(pe + 22);
    if (image.machine != kMachineI386) {
        return std::unexpected(std::format("unsupported machine 0x{:04x}, expected i386", image.machine));
    }

    size_t opt = pe + 24;
    if (r.u16(opt) != kOptionalMagicPe32) {
        return std::unexpected("optional header is not PE32");
    }
    if (optional_size < 96) {
        return std::unexpected("optional header is too small");
    }
    image.entry_rva = r.u32(opt + 16);
    image.image_base = r.u32(opt + 28);
    image.section_alignment = r.u32(opt + 32);
    image.file_alignment = r.u32(opt + 36);
    image.size_of_image = r.u32(opt + 56);
    image.size_of_headers = r.u32(opt + 60);
    image.subsystem = r.u16(opt + 68);
    uint32_t directory_count = std::min<uint32_t>(r.u32(opt + 92), 16);
    if (96 + directory_count * 8 > optional_size) {
        return std::unexpected("data directories exceed the optional header");
    }
    for (uint32_t i = 0; i < directory_count; ++i) {
        image.directories.push_back({r.u32(opt + 96 + i * 8), r.u32(opt + 100 + i * 8)});
    }

    size_t table = opt + optional_size;
    for (uint16_t i = 0; i < section_count; ++i) {
        size_t s = table + static_cast<size_t>(i) * 40;
        Section section;
        section.name = r.fixed_string(s, 8);
        section.virtual_size = r.u32(s + 8);
        section.virtual_address = r.u32(s + 12);
        section.raw_size = r.u32(s + 16);
        section.raw_offset = r.u32(s + 20);
        section.characteristics = r.u32(s + 36);
        if (r.failed()) {
            return std::unexpected("section table is truncated");
        }
        if (section.raw_size != 0 && !r.has(section.raw_offset, section.raw_size)) {
            return std::unexpected(std::format("section {} raw data lies outside the file", section.name));
        }
        image.sections.push_back(std::move(section));
    }
    if (r.failed()) {
        return std::unexpected("headers are truncated");
    }

    auto imports = parse_imports(image, r);
    if (!imports) {
        return std::unexpected(imports.error());
    }
    image.imports = std::move(*imports);
    return image;
}

std::expected<std::vector<uint8_t>, std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", path.string()));
    }
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) {
        return std::unexpected(std::format("cannot size {}", path.string()));
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(std::format("cannot read {}", path.string()));
    }
    return bytes;
}

std::optional<std::string_view> known_ordinal_name(std::string_view dll, uint16_t ordinal) {
    if (iequals(dll, "WS2_32.dll") || iequals(dll, "WSOCK32.dll")) {
        for (const auto& entry : kWs2_32Ordinals) {
            if (entry.ordinal == ordinal) {
                return entry.name;
            }
        }
    }
    return std::nullopt;
}

}
