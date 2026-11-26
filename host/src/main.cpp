#include "crash.hpp"
#include "difftest.hpp"
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
#include "macandreas/interp.hpp"
#include "macandreas/pe_image.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ma::host;

struct Options {
    fs::path game_dir;
    fs::path native;
    fs::path manifest;
    bool trace_imports = false;
    bool trace_interp = false;
    std::vector<uint32_t> interpret;
    std::vector<uint32_t> trace_pcs;
    std::vector<std::pair<uint32_t, uint32_t>> interpret_ranges;
    DiffTestOptions diff;
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
        } else if (a == "--trap-nan") {
            ma::interp::set_nan_trap(true);
        } else if (a == "--interpret-range" && i + 1 < argc) {
            std::string_view list = argv[++i];
            while (!list.empty()) {
                auto comma = list.find(',');
                std::string item(list.substr(0, comma));
                auto dash = item.find('-');
                if (dash == std::string::npos) {
                    return false;
                }
                o.interpret_ranges.emplace_back(static_cast<uint32_t>(std::stoul(item.substr(0, dash), nullptr, 16)),
                                                static_cast<uint32_t>(std::stoul(item.substr(dash + 1), nullptr, 16)));
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
        } else if (a == "--diff-test" && i + 1 < argc) {
            o.diff.list = argv[++i];
        } else if (a == "--diff-iterations" && i + 1 < argc) {
            o.diff.iterations = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--diff-x87-args" && i + 1 < argc) {
            o.diff.x87_args = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--diff-deep") {
            o.diff.deep = true;
        } else if (a == "--diff-verbose") {
            o.diff.verbose = true;
        } else if (a == "--diff-seed" && i + 1 < argc) {
            o.diff.seed = std::stoull(argv[++i], nullptr, 0);
        } else if ((a == "--interpret" || a == "--trace-pc" || a == "--watch") && i + 1 < argc) {
            auto& out = a == "--interpret" ? o.interpret : a == "--trace-pc" ? o.trace_pcs : settings().watch;
            std::string_view list = argv[++i];
            while (!list.empty()) {
                auto comma = list.find(',');
                out.push_back(static_cast<uint32_t>(std::stoul(std::string(list.substr(0, comma)), nullptr, 16)));
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
        } else if (a == "--resolution" && i + 1 < argc) {
            DisplayMode mode;
            if (!parse_mode(argv[++i], mode)) {
                return false;
            }
            settings().width = mode.width;
            settings().height = mode.height;
        } else if (a == "--modes" && i + 1 < argc) {
            std::string_view list = argv[++i];
            while (!list.empty()) {
                auto comma = list.find(',');
                DisplayMode mode;
                if (!parse_mode(list.substr(0, comma), mode)) {
                    return false;
                }
                settings().modes.push_back(mode);
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
        } else if (a == "--skip-intro") {
            settings().skip_intro = true;
        } else if (a == "--ignore-focus-loss") {
            settings().ignore_focus_loss = true;
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
        } else if (a == "--dump-shaders" && i + 1 < argc) {
            settings().shader_dump_dir = argv[++i];
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
                     "[--trace-imports] [--trace-interp] [--interpret ADDR,...] [--interpret-range LO-HI,...] [--trace-pc ADDR,...] [--trap-nan] [--watch ADDR,...] [--diff-test LIST [--diff-iterations N] [--diff-seed N] [--diff-x87-args N] [--diff-deep] [--diff-verbose]] [--resolution WxH] [--modes WxH,...] [--skip-intro] [--ignore-focus-loss] [--capture-dir DIR] "
                     "[--capture-frames N,M,...|all] [--exit-after-frames N] [--keys FRAME[m]:KEY[+|-],...] [--dump-shaders DIR]");
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

    for (uint32_t addr : options.interpret) {
        force_interpret(addr);
    }
    for (auto [low, high] : options.interpret_ranges) {
        std::println(stderr, "sa-host: interpreting {} traces in 0x{:08x}-0x{:08x}", force_interpret_range(low, high), low,
                     high);
    }
    for (uint32_t addr : options.trace_pcs) {
        trace_interpreted_pc(addr);
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

    if (!options.diff.list.empty()) {
        return run_diff_tests(*image, options.diff);
    }

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
