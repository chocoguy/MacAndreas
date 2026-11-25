#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace ma::exe {

inline constexpr std::string_view kSupportedSha256 = "a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26";
inline constexpr uint64_t kSupportedSize = 14383616;

enum class Support {
    supported,
    unsupported_known,
    unknown,
};

struct Identity {
    Support support = Support::unknown;
    std::string build;
    std::string sha256;
    uint64_t size = 0;
    std::string message;
};

std::string sha256_hex(std::span<const uint8_t> data);

Identity identify(std::span<const uint8_t> data);

std::expected<std::filesystem::path, std::string> locate_exe(const std::filesystem::path& file_or_dir);

std::string_view support_name(Support s);

}
