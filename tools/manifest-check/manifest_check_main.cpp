#include "macandreas/manifest.hpp"
#include "macandreas/settings.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <print>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> dirs;
    std::filesystem::path schema_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--schema" && i + 1 < argc) {
            schema_path = argv[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            std::println("usage: manifest-check [--schema mods/settings.yaml] DIR...\n"
                         "Loads every .yaml manifest in each DIR as one manifest and runs the strict checks\n"
                         "that need no game data: schema, overlaps, group consistency and native func_replace\n"
                         "ownership. With --schema, every setting must be a key in the settings schema.\n"
                         "sa-recomp repeats them with the analysis data and the exe at conversion.");
            return 0;
        }
        dirs.emplace_back(arg);
    }
    if (dirs.empty()) {
        std::println(stderr, "usage: manifest-check [--schema FILE] DIR...");
        return 2;
    }
    ma::manifest::Manifest all;
    bool failed = false;
    for (const auto& dir : dirs) {
        if (!std::filesystem::is_directory(dir)) {
            std::println(stderr, "manifest-check: {} is not a directory", dir.string());
            return 2;
        }
        auto loaded = ma::manifest::load_directory(dir);
        if (!loaded) {
            for (const auto& d : loaded.error()) {
                std::println(stderr, "{}", d.text());
            }
            failed = true;
            continue;
        }
        for (auto& e : loaded->entries) {
            all.entries.push_back(std::move(e));
        }
    }
    if (failed) {
        return 1;
    }
    ma::manifest::CheckContext context;
    ma::settings::Schema schema;
    if (!schema_path.empty()) {
        auto loaded = ma::settings::load_schema(schema_path);
        if (!loaded) {
            for (const auto& d : loaded.error()) {
                std::println(stderr, "{}", d.text());
            }
            return 1;
        }
        schema = std::move(*loaded);
        context.is_known_setting = [&](std::string_view key) { return schema.find(key) != nullptr; };
    }
    auto problems = ma::manifest::check(all, context);
    for (const auto& d : problems) {
        std::println(stderr, "{}", d.text());
    }
    std::map<std::string, size_t> by_source;
    for (const auto& e : all.entries) {
        ++by_source[std::string(ma::manifest::to_string(e.source))];
    }
    std::string summary;
    for (const auto& [source, count] : by_source) {
        summary += std::format("{}{} {}", summary.empty() ? "" : ", ", count, source);
    }
    std::println("manifest-check: {} entries ({}), {} problem(s)", all.entries.size(), summary.empty() ? "none" : summary,
                 problems.size());
    return problems.empty() ? 0 : 1;
}
