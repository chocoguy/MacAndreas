#include "json_writer.hpp"

#include "macandreas/exe_identity.hpp"
#include "macandreas/pe_image.hpp"

#include <cstdio>
#include <print>
#include <string>
#include <string_view>

namespace {

enum Exit : int {
    ok = 0,
    error = 1,
    unsupported = 2,
    unknown = 3,
    usage = 64,
};

int exit_for(ma::exe::Support s) {
    switch (s) {
        case ma::exe::Support::supported:
            return ok;
        case ma::exe::Support::unsupported_known:
            return unsupported;
        case ma::exe::Support::unknown:
            return unknown;
    }
    return unknown;
}

}

int main(int argc, char** argv) {
    bool json = false;
    std::string target;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--json") {
            json = true;
        } else if (target.empty()) {
            target = arg;
        } else {
            target.clear();
            break;
        }
    }
    if (target.empty()) {
        std::println(stderr, "usage: sa-verify [--json] <gta_sa.exe | game directory>");
        return usage;
    }

    auto exe = ma::exe::locate_exe(target);
    if (!exe) {
        std::println(stderr, "sa-verify: {}", exe.error());
        return error;
    }
    auto bytes = ma::pe::read_file(*exe);
    if (!bytes) {
        std::println(stderr, "sa-verify: {}", bytes.error());
        return error;
    }
    auto id = ma::exe::identify(*bytes);

    if (json) {
        std::println("{}", ma::tools::JsonObject()
                               .str("path", exe->string())
                               .str("status", ma::exe::support_name(id.support))
                               .str("build", id.build)
                               .num("size", id.size)
                               .str("sha256", id.sha256)
                               .str("message", id.message)
                               .build());
    } else {
        std::println("{}", exe->string());
        std::println("  build   {}", id.build);
        std::println("  size    {} bytes", id.size);
        std::println("  sha256  {}", id.sha256);
        std::println("  status  {}", ma::exe::support_name(id.support));
        if (id.support != ma::exe::Support::supported) {
            std::println("");
            std::println("{}", id.message);
        }
    }
    return exit_for(id.support);
}
