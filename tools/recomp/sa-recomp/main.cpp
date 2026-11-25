#include "compile.hpp"
#include "lift.hpp"

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

struct Args {
    std::string command;
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;

    std::optional<std::string> get(const std::string& key) const {
        auto it = options.find(key);
        return it == options.end() ? std::nullopt : std::optional(it->second);
    }
};

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc > 1) {
        args.command = argv[1];
    }
    for (int i = 2; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.starts_with("--")) {
            auto eq = a.find('=');
            if (eq != std::string_view::npos) {
                args.options[std::string(a.substr(2, eq - 2))] = std::string(a.substr(eq + 1));
            } else if (i + 1 < argc) {
                args.options[std::string(a.substr(2))] = argv[++i];
            } else {
                args.options[std::string(a.substr(2))] = "";
            }
        } else {
            args.positional.emplace_back(a);
        }
    }
    return args;
}

size_t to_size(const std::optional<std::string>& s, size_t fallback) {
    if (!s) {
        return fallback;
    }
    size_t v = fallback;
    std::from_chars(s->data(), s->data() + s->size(), v);
    return v;
}

std::vector<uint32_t> parse_addresses(const std::optional<std::string>& s) {
    std::vector<uint32_t> out;
    if (!s) {
        return out;
    }
    std::string_view rest = *s;
    while (!rest.empty()) {
        auto comma = rest.find(',');
        auto item = rest.substr(0, comma);
        if (item.starts_with("0x")) {
            item.remove_prefix(2);
        }
        uint32_t v = 0;
        std::from_chars(item.data(), item.data() + item.size(), v, 16);
        out.push_back(v);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return out;
}

fs::path self_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    _NSGetExecutablePath(buf.data(), &size);
    return fs::canonical(buf.c_str());
}

pid_t spawn(const std::vector<std::string>& argv) {
    std::vector<char*> raw;
    for (const auto& a : argv) {
        raw.push_back(const_cast<char*>(a.c_str()));
    }
    raw.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, raw[0], nullptr, nullptr, raw.data(), environ) != 0) {
        return -1;
    }
    return pid;
}

int wait_for(pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int run(const std::vector<std::string>& argv) {
    pid_t pid = spawn(argv);
    return pid < 0 ? 127 : wait_for(pid);
}

int compile_parallel(const std::vector<fs::path>& inputs, const fs::path& runtime, size_t jobs, int opt) {
    auto self = self_path().string();
    std::deque<fs::path> queue(inputs.begin(), inputs.end());
    std::map<pid_t, fs::path> running;
    size_t done = 0;
    int failures = 0;
    auto started = std::chrono::steady_clock::now();
    while (!queue.empty() || !running.empty()) {
        while (!queue.empty() && running.size() < jobs) {
            auto input = queue.front();
            queue.pop_front();
            auto output = fs::path(input).replace_extension(".o");
            pid_t pid = spawn({self, "compile", "--runtime", runtime.string(), "--opt", std::to_string(opt), "--out",
                               output.string(), input.string()});
            if (pid < 0) {
                std::println(stderr, "sa-recomp: cannot spawn compiler for {}", input.string());
                ++failures;
                continue;
            }
            running[pid] = input;
        }
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) {
            break;
        }
        auto it = running.find(pid);
        if (it == running.end()) {
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::println(stderr, "\nsa-recomp: compiling {} failed", it->second.string());
            ++failures;
        }
        running.erase(it);
        ++done;
        auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "\rsa-recomp: compiled %zu/%zu modules (%.0fs)", done, inputs.size(), secs);
    }
    std::fprintf(stderr, "\n");
    return failures;
}

int link_dylib(const fs::path& dir, const fs::path& output) {
    std::vector<std::string> objects;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".o") {
            objects.push_back(entry.path().string());
        }
    }
    std::ranges::sort(objects);
    if (objects.empty()) {
        std::println(stderr, "sa-recomp: no objects to link in {}", dir.string());
        return 1;
    }
    std::vector<std::string> argv = {"/usr/bin/xcrun", "clang", "-arch", "arm64", "-mmacosx-version-min=26.0",
                                     "-dynamiclib", "-o", output.string(), "-install_name",
                                     "@rpath/" + output.filename().string(), "-Wl,-undefined,dynamic_lookup",
                                     "-Wl,-no_warn_duplicate_libraries"};
    argv.insert(argv.end(), objects.begin(), objects.end());
    return run(argv);
}

void usage() {
    std::println(stderr,
                 "usage:\n"
                 "  sa-recomp build   --exe <gta_sa.exe> --analysis <dir> --runtime <guest_intrinsics.bc> --out <dir>\n"
                 "                    [--jobs N] [--shard-size N] [--limit N] [--only 0xADDR,...] [--opt N] [--manifest <dir>]\n"
                 "  sa-recomp lift    --exe <gta_sa.exe> --analysis <dir> --out <dir> [--shard-size N] [--limit N]\n"
                 "  sa-recomp compile --runtime <bc> --out <file.o> [--opt N] [--emit-ir] <module.bc>\n"
                 "  sa-recomp link    --out <dir>");
}

int cmd_lift(const Args& args, ma::recomp::LiftResult* out) {
    auto exe = args.get("exe");
    auto analysis = args.get("analysis");
    auto dir = args.get("out");
    if (!exe || !analysis || !dir) {
        usage();
        return 64;
    }
    ma::recomp::LiftOptions options;
    options.exe = *exe;
    options.analysis_dir = *analysis;
    options.out_dir = fs::path(*dir) / "shards";
    options.traces_per_shard = to_size(args.get("shard-size"), 400);
    options.limit = to_size(args.get("limit"), 0);
    options.only = parse_addresses(args.get("only"));
    if (auto manifest = args.get("manifest")) {
        options.manifest_dir = *manifest;
    }
    if (fs::exists(options.out_dir)) {
        fs::remove_all(options.out_dir);
    }
    auto result = ma::recomp::lift_program(options);
    if (!result) {
        std::println(stderr, "sa-recomp: {}", result.error());
        return 1;
    }
    std::println(stderr, "sa-recomp: {} roots, {} traces lifted, {} unresolved trace stubs, {} shards", result->roots,
                 result->traces, result->stubs, result->shards.size());
    if (!result->unresolved_jumps.empty()) {
        auto path = fs::path(*dir) / "unresolved_jumps.txt";
        std::ofstream list(path);
        for (uint32_t site : result->unresolved_jumps) {
            list << std::format("0x{:08x}\n", site);
        }
        std::println(stderr, "sa-recomp: {} computed jumps still unresolved, listed in {}", result->unresolved_jumps.size(),
                     path.string());
    }
    if (out) {
        *out = *result;
    }
    return 0;
}

int cmd_compile(const Args& args) {
    auto output = args.get("out");
    if (!output || args.positional.size() != 1) {
        usage();
        return 64;
    }
    ma::recomp::CompileOptions options;
    options.input = args.positional[0];
    options.output = *output;
    options.runtime_bc = args.get("runtime").value_or("");
    options.opt_level = static_cast<int>(to_size(args.get("opt"), 2));
    options.emit_ir = args.options.contains("emit-ir");
    auto result = ma::recomp::compile_module(options);
    if (!result) {
        std::println(stderr, "sa-recomp: {}", result.error());
        return 1;
    }
    return 0;
}

int cmd_link(const Args& args) {
    auto dir = args.get("out");
    if (!dir) {
        usage();
        return 64;
    }
    auto output = fs::path(*dir) / "libgta_sa_native.dylib";
    int rc = link_dylib(fs::path(*dir) / "shards", output);
    if (rc == 0) {
        std::println(stderr, "sa-recomp: wrote {}", output.string());
    }
    return rc;
}

int cmd_recompile(const Args& args) {
    auto runtime = args.get("runtime");
    auto dir = args.get("out");
    if (!runtime || !dir) {
        usage();
        return 64;
    }
    std::vector<fs::path> inputs;
    for (const auto& entry : fs::directory_iterator(fs::path(*dir) / "shards")) {
        if (entry.path().extension() == ".bc") {
            inputs.push_back(entry.path());
        }
    }
    std::ranges::sort(inputs);
    size_t jobs = to_size(args.get("jobs"), std::max(1u, std::thread::hardware_concurrency()));
    int opt = static_cast<int>(to_size(args.get("opt"), 2));
    if (int failures = compile_parallel(inputs, *runtime, jobs, opt); failures != 0) {
        std::println(stderr, "sa-recomp: {} module(s) failed to compile", failures);
        return 1;
    }
    return cmd_link(args);
}

int cmd_build(const Args& args) {
    auto runtime = args.get("runtime");
    if (!runtime) {
        usage();
        return 64;
    }
    ma::recomp::LiftResult lifted;
    if (int rc = cmd_lift(args, &lifted); rc != 0) {
        return rc;
    }
    std::vector<fs::path> inputs = lifted.shards;
    inputs.push_back(lifted.functable);
    size_t jobs = to_size(args.get("jobs"), std::max(1u, std::thread::hardware_concurrency()));
    int opt = static_cast<int>(to_size(args.get("opt"), 2));
    if (int failures = compile_parallel(inputs, *runtime, jobs, opt); failures != 0) {
        std::println(stderr, "sa-recomp: {} module(s) failed to compile", failures);
        return 1;
    }
    return cmd_link(args);
}

}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    if (args.command == "build") {
        return cmd_build(args);
    }
    if (args.command == "lift") {
        return cmd_lift(args, nullptr);
    }
    if (args.command == "compile") {
        return cmd_compile(args);
    }
    if (args.command == "link") {
        return cmd_link(args);
    }
    if (args.command == "recompile") {
        return cmd_recompile(args);
    }
    usage();
    return 64;
}
