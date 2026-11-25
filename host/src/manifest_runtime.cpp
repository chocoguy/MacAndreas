#include "manifest_runtime.hpp"

#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "native/natives.hpp"

#include <cstdio>
#include <cstring>
#include <format>

namespace ma::host {

std::expected<ManifestStats, std::string> apply_runtime_manifest(const manifest::Manifest& m) {
    ManifestStats stats;
    std::string errors;
    for (const auto& e : m.entries) {
        switch (e.kind) {
        case manifest::Kind::func_replace: {
            if (e.source != manifest::Source::native) {
                errors += std::format("{}:{}: func_replace from {} has no ported native code yet\n", e.file, e.line,
                                      manifest::to_string(e.source));
                break;
            }
            TraceFn fn = native::find_symbol(e.target);
            if (!fn) {
                errors += std::format("{}:{}: unknown native symbol \"{}\"\n", e.file, e.line, e.target);
                break;
            }
            register_trace(e.addr, fn);
            std::fprintf(stderr, "[MANIFEST] func_replace 0x%08x -> %s (%s)\n", e.addr, e.target.c_str(),
                         e.group.c_str());
            ++stats.replaced;
            break;
        }
        case manifest::Kind::data_poke:
            std::memcpy(mem::ptr(e.addr), e.bytes.data(), e.bytes.size());
            ++stats.poked;
            break;
        default:
            ++stats.lift_time;
            break;
        }
    }
    if (!errors.empty()) {
        return std::unexpected(errors);
    }
    return stats;
}

}
