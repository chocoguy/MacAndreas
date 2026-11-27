#include "mod_settings.hpp"

#include <format>

namespace ma::host {

namespace {

settings::Schema g_schema;
settings::Values g_values{&g_schema};

std::string join(const std::vector<settings::Diagnostic>& errors) {
    std::string out;
    for (const auto& d : errors) {
        out += (out.empty() ? "" : "\n") + d.text();
    }
    return out;
}

}

std::expected<void, std::string> init_mod_settings(const std::filesystem::path& schema, const std::filesystem::path& values,
                                                   const std::vector<std::string>& overrides) {
    if (!schema.empty()) {
        auto loaded = settings::load_schema(schema);
        if (!loaded) {
            return std::unexpected(join(loaded.error()));
        }
        g_schema = std::move(*loaded);
    }
    g_values = settings::Values(&g_schema);
    if (!values.empty() && std::filesystem::exists(values)) {
        auto loaded = settings::load_values(values, g_schema);
        if (!loaded) {
            return std::unexpected(join(loaded.error()));
        }
        g_values = std::move(*loaded);
    }
    for (const auto& item : overrides) {
        auto eq = item.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(std::format("--set {}: expected key=value", item));
        }
        if (auto r = g_values.set(item.substr(0, eq), item.substr(eq + 1)); !r) {
            return std::unexpected(std::format("--set {}: {}", item, r.error()));
        }
    }
    return {};
}

const settings::Values& mod_settings() { return g_values; }

const settings::Schema& mod_schema() { return g_schema; }

}
