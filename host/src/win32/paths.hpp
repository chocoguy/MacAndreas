#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ma::host::win32 {

inline constexpr std::string_view kGuestDocuments = "C:\\Users\\Player\\Documents";

std::string normalize_guest_path(std::string_view path, std::string_view cwd);
std::optional<std::filesystem::path> resolve_case(const std::filesystem::path& base, std::string_view relative);
std::optional<std::filesystem::path> host_path_for(std::string_view guest_path);
std::string current_directory();
void set_current_directory(std::string guest_path);
bool wildcard_match(std::string_view pattern, std::string_view name);

}
