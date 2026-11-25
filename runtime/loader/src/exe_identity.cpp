#include "macandreas/exe_identity.hpp"

#include "macandreas/pe_image.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>

namespace ma::exe {

namespace {

struct KnownBuild {
    std::string_view name;
    uint64_t size;
    std::string_view sha256;
    Support support;
};

constexpr std::array kKnownBuilds = {
    KnownBuild{"1.0 US", kSupportedSize, kSupportedSha256, Support::supported},
    KnownBuild{"Steam release", 5971456, "15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb",
               Support::unsupported_known},
};

constexpr std::string_view kRequirement =
    "MacAndreas requires the GTA: San Andreas 1.0 US executable (gta_sa.exe, 14,383,616 bytes).";

constexpr std::string_view kDowngradeAdvice =
    "Downgrade your installation to 1.0 US, then select the resulting gta_sa.exe.";

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<std::filesystem::path> find_case_insensitive(const std::filesystem::path& dir, std::string_view name) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && lower(entry.path().filename().string()) == name) {
            return entry.path();
        }
    }
    return std::nullopt;
}

}

std::string sha256_hex(std::span<const uint8_t> data) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    constexpr size_t kChunk = 1u << 20;
    for (size_t offset = 0; offset < data.size(); offset += kChunk) {
        size_t n = std::min(kChunk, data.size() - offset);
        CC_SHA256_Update(&ctx, data.data() + offset, static_cast<CC_LONG>(n));
    }
    std::array<uint8_t, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &ctx);
    std::string out;
    out.reserve(digest.size() * 2);
    for (uint8_t b : digest) {
        out += std::format("{:02x}", b);
    }
    return out;
}

Identity identify(std::span<const uint8_t> data) {
    Identity id;
    id.size = data.size();
    id.sha256 = sha256_hex(data);
    for (const auto& known : kKnownBuilds) {
        if (known.size == id.size && known.sha256 == id.sha256) {
            id.support = known.support;
            id.build = known.name;
            id.message = known.support == Support::supported
                             ? std::string("Supported executable.")
                             : std::format("This is the {} of GTA: San Andreas, which MacAndreas does not support. {}",
                                           known.name, kDowngradeAdvice);
            return id;
        }
    }
    id.support = Support::unknown;
    auto image = pe::parse(data);
    if (!image) {
        id.build = "not a PE executable";
        id.message = std::format("{} This file is not a Windows executable ({}).", kRequirement, image.error());
    } else if (id.size == kSupportedSize) {
        id.build = "modified 1.0 US";
        id.message = std::format("{} This file has the 1.0 US size but different contents; it has been modified. "
                                 "Restore an unmodified gta_sa.exe.",
                                 kRequirement);
    } else {
        id.build = "unrecognized";
        id.message = std::format("{} This executable is not recognized (size {} bytes). If it is another release of "
                                 "the game: {}",
                                 kRequirement, id.size, kDowngradeAdvice);
    }
    return id;
}

std::expected<std::filesystem::path, std::string> locate_exe(const std::filesystem::path& file_or_dir) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(file_or_dir, ec)) {
        return file_or_dir;
    }
    if (!std::filesystem::is_directory(file_or_dir, ec)) {
        return std::unexpected(std::format("{} does not exist", file_or_dir.string()));
    }
    for (std::string_view candidate : {"gta_sa.exe", "gta-sa.exe"}) {
        if (auto found = find_case_insensitive(file_or_dir, candidate)) {
            return *found;
        }
    }
    return std::unexpected(std::format("no gta_sa.exe found in {}", file_or_dir.string()));
}

std::string_view support_name(Support s) {
    switch (s) {
        case Support::supported:
            return "supported";
        case Support::unsupported_known:
            return "unsupported";
        case Support::unknown:
            return "unknown";
    }
    return "unknown";
}

}
