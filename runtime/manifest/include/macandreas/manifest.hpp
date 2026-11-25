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

enum class Kind { byte_patch, nop, data_poke, call_redirect, mid_hook, func_replace };
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
};

std::vector<Diagnostic> check(const Manifest& manifest, const CheckContext& context = {});

}
