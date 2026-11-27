#pragma once

#include "macandreas/settings.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace ma::host {

std::expected<void, std::string> init_mod_settings(const std::filesystem::path& schema, const std::filesystem::path& values,
                                                   const std::vector<std::string>& overrides);
const settings::Values& mod_settings();
const settings::Schema& mod_schema();

}
