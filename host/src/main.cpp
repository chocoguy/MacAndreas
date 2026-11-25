#include "crash.hpp"
#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "input/keys.hpp"
#include "manifest_runtime.hpp"
#include "settings.hpp"
#include "thread.hpp"
#include "thunks.hpp"
#include "win32/process.hpp"
#include "win32/seh.hpp"

#include "macandreas/exe_identity.hpp"
#include "macandreas/pe_image.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace ma::host;

struct Options {
    fs::path game_dir;
    fs::path native;
    fs::path manifest;
    bool trace_imports = false;
    bool trace_interp = false;
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--game" && i + 1 < argc) {
            o.game_dir = argv[++i];
        } else if (a == "--native" && i + 1 < argc) {
            o.native = argv[++i];
        } else if (a == "--manifest" && i + 1 < argc) {
            o.manifest = argv[++i];
        } else if (a == "--trace-imports") {
            o.trace_imports = true;
        } else if (a == "--trace-interp") {
            o.trace_interp = true;
        } else if (a == "--resolution" && i + 1 < argc) {
            unsigned w = 0;
            unsigned h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || w < 320 || h < 240) {
                return false;
            }
            settings().width = w;
            settings().height = h;
        } else if (a == "--skip-intro") {
            settings().skip_intro = true;
        } else if (a == "--capture-dir" && i + 1 < argc) {
            settings().capture_dir = argv[++i];
        } else if (a == "--capture-frames" && i + 1 < argc) {
            std::string_view list = argv[++i];
            if (list == "all") {
                settings().capture_all = true;
                continue;
            }
            while (!list.empty()) {
                auto comma = list.find(',');
                auto item = list.substr(0, comma);
                settings().capture_frames.push_back(static_cast<uint32_t>(std::stoul(std::string(item))));
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
        } else if (a == "--exit-after-frames" && i + 1 < argc) {
            settings().exit_after_frames = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--keys" && i + 1 < argc) {
            settings().keys = argv[++i];
        } else {
            return false;
        }
    }
    return !o.game_dir.empty() && !o.native.empty();
}

size_t resolve_imports(const ma::pe::Image& image) {
    size_t count = 0;
    for (const auto& module : image.imports) {
        for (const auto& entry : module.entries) {
            uint32_t thunk = thunk_for(module.dll, entry.ordinal ? "" : entry.name, entry.ordinal.value_or(0));
            mem::write<uint32_t>(image.image_base + entry.iat_rva, thunk);
            ++count;
        }
    }
    return count;
}

}

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::println(stderr, "usage: sa-host --game <game dir> --native <libgta_sa_native.dylib> [--manifest <dir>] "
                     "[--trace-imports] [--trace-interp] [--resolution WxH] [--skip-intro] [--capture-dir DIR] "
                     "[--capture-frames N,M,...|all] [--exit-after-frames N] [--keys FRAME[m]:KEY[+|-],...]");
        return 64;
    }
    if (!settings().keys.empty()) {
        std::string error;
        auto steps = ma::host::input::parse_key_script(settings().keys, error);
        if (steps.empty()) {
            std::println(stderr, "sa-host: --keys: {}", error);
            return 64;
        }
        ma::host::input::set_key_script(std::move(steps));
    }
    auto exe_path = ma::exe::locate_exe(options.game_dir / "gta_sa.exe");
    if (!exe_path) {
        std::println(stderr, "sa-host: {}", exe_path.error());
        return 1;
    }
    auto bytes = ma::pe::read_file(*exe_path);
    if (!bytes) {
        std::println(stderr, "sa-host: {}", bytes.error());
        return 1;
    }
    auto id = ma::exe::identify(*bytes);
    if (id.support != ma::exe::Support::supported) {
        std::println(stderr, "sa-host: {}", id.message);
        return 1;
    }
    auto image = ma::pe::parse(*bytes);
    if (!image) {
        std::println(stderr, "sa-host: {}", image.error());
        return 1;
    }

    if (!reserve_guest_space()) {
        std::println(stderr, "sa-host: cannot reserve the 4 GB guest address space");
        return 1;
    }
    if (auto mapped = map_image(*image, *bytes); !mapped) {
        std::println(stderr, "sa-host: {}", mapped.error());
        return 1;
    }
    install_crash_handler();

    void* lib = dlopen(options.native.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::println(stderr, "sa-host: {}", dlerror());
        return 1;
    }
    auto traces = load_function_table(lib);
    if (!traces) {
        std::println(stderr, "sa-host: {}", traces.error());
        return 1;
    }

    set_trace_imports(options.trace_imports);
    set_trace_interp(options.trace_interp);
    if (!options.manifest.empty()) {
        auto loaded = ma::manifest::load_directory(options.manifest);
        if (!loaded) {
            for (const auto& d : loaded.error()) {
                std::println(stderr, "sa-host: manifest: {}", d.text());
            }
            return 1;
        }
        auto problems = ma::manifest::check(*loaded);
        for (const auto& d : problems) {
            std::println(stderr, "sa-host: manifest: {}", d.text());
        }
        if (!problems.empty()) {
            return 1;
        }
        auto applied = apply_runtime_manifest(*loaded);
        if (!applied) {
            std::print(stderr, "sa-host: manifest:\n{}", applied.error());
            return 1;
        }
        std::println(stderr, "sa-host: manifest: {} native replacements, {} data pokes, {} lift-time entries",
                     applied->replaced, applied->poked, applied->lift_time);
    }
    win32::register_all();
    win32::init_process(options.game_dir, image->image_base);
    init_process_block(image->image_base, 0);
    size_t imports = resolve_imports(*image);

    GuestThread* main_thread = create_guest_thread(kMainStackBottom, kMainStackTop);
    bind_current_thread(main_thread);

    std::println(stderr, "sa-host: guest base {}, {} traces, {} imports, entry 0x{:08x}",
                 static_cast<void*>(ma_guest_base), *traces, imports, image->entry_va());

    GuestState& s = main_thread->state;
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), kReturnSentinel);
    try {
        dispatch(s, image->entry_va());
        finish_call(s, kReturnSentinel);
    } catch (const win32::GuestUnwind& unwind) {
        std::println(stderr, "[SEH] unwind to 0x{:08x} esp=0x{:08x} escaped every guest frame", unwind.eip, unwind.esp);
        return 1;
    }
    std::println(stderr, "sa-host: entry point returned eax=0x{:08x}", eax(s));
    return static_cast<int>(eax(s));
}
