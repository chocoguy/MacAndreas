#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ma::manifest {

inline constexpr uint32_t kRedirectBase = 0xE0000000;

constexpr uint32_t redirect_address(uint32_t site) { return kRedirectBase + site; }

inline constexpr uint32_t kOperandSlotBase = 0xE8000000;
inline constexpr uint32_t kOperandSlotLimit = 0x02000000;

constexpr uint32_t operand_slot_address(uint32_t site) { return kOperandSlotBase + 4 * site; }

constexpr bool is_operand_slot_instruction(uint8_t opcode, uint8_t modrm) {
    return (modrm & 0xC7) == 0x05 && (opcode == 0xD8 || (opcode == 0xD9 && (modrm & 0x38) == 0));
}

enum class Kind { byte_patch, nop, data_poke, call_redirect, mid_hook, func_replace, operand_slot, data_slot, marker };
enum class Source { silentpatch, widescreen, project2dfx, native };

std::string_view to_string(Kind kind);
std::string_view to_string(Source source);

struct Entry {
    uint32_t addr = 0;
    Kind kind = Kind::byte_patch;
    Source source = Source::native;
    std::string group;
    std::string target;
    std::vector<uint8_t> bytes;
    uint32_t len = 0;
    bool tso = false;
    std::string setting;
    std::vector<uint32_t> resume;
    std::string site;
    std::string notes;
    std::string file;
    int line = 0;

    uint32_t span() const;
    bool lift_time() const;
};

struct Diagnostic {
    std::string file;
    int line = 0;
    std::string message;

    std::string text() const;
};

struct Manifest {
    std::vector<Entry> entries;
};

std::expected<Manifest, std::vector<Diagnostic>> parse(std::string_view text, const std::string& file);
std::expected<Manifest, std::vector<Diagnostic>> load_directory(const std::filesystem::path& dir);

struct CheckContext {
    std::function<bool(uint32_t)> is_instruction_start;
    std::function<bool(uint32_t)> is_function_entry;
    std::function<std::optional<uint32_t>(uint32_t)> function_end;
    std::function<bool(uint32_t)> is_call_instruction;
    std::function<bool(std::string_view)> is_known_setting;
};

std::vector<Diagnostic> check(const Manifest& manifest, const CheckContext& context = {});

}
