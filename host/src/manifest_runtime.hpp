#pragma once

#include "macandreas/manifest.hpp"

#include <expected>
#include <string>

namespace ma::host {

struct ManifestStats {
    size_t replaced = 0;
    size_t poked = 0;
    size_t lift_time = 0;
};

std::expected<ManifestStats, std::string> apply_runtime_manifest(const manifest::Manifest& manifest);

}
