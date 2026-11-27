#pragma once

#include "macandreas/manifest.hpp"

#include <expected>
#include <string>

namespace ma::host {

struct ManifestStats {
    size_t replaced = 0;
    size_t poked = 0;
    size_t lift_time = 0;
    size_t redirected = 0;
    size_t hooked = 0;
    size_t slotted = 0;
    size_t disabled = 0;
};

std::expected<ManifestStats, std::string> apply_runtime_manifest(const manifest::Manifest& manifest);
void refresh_operand_slots();

}
