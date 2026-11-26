#include "difftest.hpp"

#include "abi.hpp"
#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "thread.hpp"
#include "thunks.hpp"

#include "macandreas/interp.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace ma::host {

namespace {

constexpr uint32_t kScratchSize = 0x40000;
constexpr uint32_t kBufferSize = 0x800;
constexpr uint32_t kBufferCount = 24;
constexpr uint32_t kStackTop = 0x3f000;
constexpr uint32_t kArgSlots = 16;
constexpr unsigned kTimeoutSeconds = 2;
constexpr std::array<uint16_t, 3> kControlWords = {0x027f, 0x007f, 0x037f};

enum class Status : uint32_t {
    ok = 0,
    bad_return = 1,
    unsupported = 2,
    handed_off = 3,
    no_report = 4,
    signaled = 100,
};

struct Report {
    Status status = Status::no_report;
    uint32_t detail = 0;
    std::array<uint32_t, 8> gpr{};
    uint32_t eip = 0;
    uint32_t top = 0;
    std::array<uint64_t, 8> st{};
    uint64_t mem_hash = 0;
    uint32_t mem_changes = 0;
    uint32_t first_change = 0;
};

struct Region {
    uint32_t addr;
    uint32_t size;
};

enum class Mode { lifted, interpreted };

struct Case {
    GuestState state;
    uint32_t entry_esp;
    bool deep;
};

std::array<uint32_t, 8> gprs(GuestState& s) {
    return {eax(s), ecx(s), edx(s), ebx(s), esp(s), ebp(s), esi(s), edi(s)};
}

uint64_t st_bits(const GuestState& s, unsigned i) {
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(&s.st.elems[i]) + 6, sizeof(v));
    return v;
}

bool interp_has_native(void*, uint32_t pc) { return is_thunk(pc); }

bool interp_has_native_deep(void*, uint32_t pc) { return is_thunk(pc); }

void interp_call_deep(void* context, GuestState& s, uint32_t target) {
    if (is_thunk(target)) {
        dispatch(s, target);
        return;
    }
    interp::Hooks hooks{context, interp_has_native_deep, interp_call_deep};
    auto result = interp::run(s, target, hooks);
    if (result.stop == interp::Stop::unsupported) {
        std::fprintf(stderr, "[DIFF] interpreter: %s\n", result.message.c_str());
        _exit(3);
    }
}

void interp_call_native(void*, GuestState& s, uint32_t target) { dispatch(s, target); }

Report execute(Mode mode, uint32_t target, const Case& c, GuestState& s, const std::vector<Region>& regions) {
    std::vector<std::vector<uint8_t>> before;
    before.reserve(regions.size());
    for (const Region& r : regions) {
        before.emplace_back(mem::ptr(r.addr), mem::ptr(r.addr) + r.size);
    }
    s = c.state;
    Report out;
    out.status = Status::ok;
    if (mode == Mode::lifted) {
        dispatch(s, target);
    } else {
        interp::Hooks hooks{nullptr, interp_has_native, c.deep ? interp_call_deep : interp_call_native};
        auto result = interp::run(s, target, hooks);
        if (result.stop == interp::Stop::unsupported) {
            out.status = Status::unsupported;
            out.detail = result.pc;
        } else if (result.stop == interp::Stop::handed_off) {
            out.status = Status::handed_off;
            out.detail = result.pc;
        }
    }
    if (out.status == Status::ok && eip(s) != kReturnSentinel) {
        out.status = Status::bad_return;
        out.detail = eip(s);
    }
    out.gpr = gprs(s);
    out.eip = eip(s);
    out.top = s.x87.fxsave32.swd.top;
    for (unsigned i = 0; i < 8; ++i) {
        out.st[i] = 0;
    }
    unsigned depth = (8 - out.top) & 7;
    for (unsigned i = 0; i < depth; ++i) {
        out.st[i] = st_bits(s, i);
    }
    uint64_t hash = 1469598103934665603ull;
    for (size_t k = 0; k < regions.size(); ++k) {
        const uint8_t* now = mem::ptr(regions[k].addr);
        for (uint32_t i = 0; i < regions[k].size; ++i) {
            if (now[i] != before[k][i]) {
                uint32_t addr = regions[k].addr + i;
                if (out.mem_changes++ == 0) {
                    out.first_change = addr;
                }
                hash = (hash ^ addr) * 1099511628211ull;
                hash = (hash ^ now[i]) * 1099511628211ull;
            }
        }
    }
    out.mem_hash = hash;
    return out;
}

Report run_isolated(Mode mode, uint32_t target, const Case& c, GuestState& s, const std::vector<Region>& regions) {
    int fds[2];
    if (pipe(fds) != 0) {
        return {};
    }
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        signal(SIGILL, SIG_DFL);
        signal(SIGFPE, SIG_DFL);
        signal(SIGTRAP, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        alarm(kTimeoutSeconds);
        Report r = execute(mode, target, c, s, regions);
        ssize_t written = write(fds[1], &r, sizeof(r));
        _exit(written == static_cast<ssize_t>(sizeof(r)) ? 0 : 1);
    }
    close(fds[1]);
    Report r;
    size_t got = 0;
    while (got < sizeof(r)) {
        ssize_t n = read(fds[0], reinterpret_cast<uint8_t*>(&r) + got, sizeof(r) - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got != sizeof(r)) {
        r = Report{};
        if (WIFSIGNALED(status)) {
            r.status = Status::signaled;
            r.detail = static_cast<uint32_t>(WTERMSIG(status));
        }
    }
    return r;
}

bool same_double(uint64_t a, uint64_t b) {
    if (a == b) {
        return true;
    }
    return std::isnan(std::bit_cast<double>(a)) && std::isnan(std::bit_cast<double>(b));
}

std::string status_text(const Report& r) {
    switch (r.status) {
    case Status::ok: return "ok";
    case Status::bad_return: return std::format("returned to 0x{:08x}", r.detail);
    case Status::unsupported: return std::format("unsupported at 0x{:08x}", r.detail);
    case Status::handed_off: return std::format("handed off at 0x{:08x}", r.detail);
    case Status::no_report: return "no report";
    case Status::signaled: return std::format("signal {}", r.detail);
    }
    return "?";
}

bool timed_out(const Report& r) { return r.status == Status::signaled && r.detail == SIGALRM; }

std::string compare(const Report& lifted, const Report& interp) {
    static constexpr const char* kNames[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    if (lifted.status != interp.status || lifted.detail != interp.detail) {
        return std::format("status lifted={} interp={}", status_text(lifted), status_text(interp));
    }
    if (lifted.status != Status::ok) {
        return {};
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (lifted.gpr[i] != interp.gpr[i]) {
            return std::format("{} lifted=0x{:08x} interp=0x{:08x}", kNames[i], lifted.gpr[i], interp.gpr[i]);
        }
    }
    if (lifted.top != interp.top) {
        return std::format("x87 top lifted={} interp={}", lifted.top, interp.top);
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (!same_double(lifted.st[i], interp.st[i])) {
            return std::format("st{} lifted={:.17g} interp={:.17g}", i, std::bit_cast<double>(lifted.st[i]),
                               std::bit_cast<double>(interp.st[i]));
        }
    }
    if (lifted.mem_changes != interp.mem_changes || lifted.mem_hash != interp.mem_hash) {
        return std::format("memory lifted={} bytes from 0x{:08x} interp={} bytes from 0x{:08x}", lifted.mem_changes,
                           lifted.first_change, interp.mem_changes, interp.first_change);
    }
    return {};
}

std::string describe(const Report& r) {
    std::string out = status_text(r);
    for (uint32_t v : r.gpr) {
        out += std::format(" {:08x}", v);
    }
    out += std::format(" top={}", r.top);
    for (unsigned i = 0; i < ((8 - r.top) & 7); ++i) {
        out += std::format(" st{}={:.17g}", i, std::bit_cast<double>(r.st[i]));
    }
    return out + std::format(" mem={}@{:08x}", r.mem_changes, r.first_change);
}

std::string describe_case(const Case& c) {
    GuestState s = c.state;
    std::string out = "  args";
    for (unsigned i = 1; i <= 6; ++i) {
        uint32_t v = mem::read<uint32_t>(c.entry_esp + 4 * i);
        out += std::format(" {:08x}({:.9g})", v, std::bit_cast<float>(v));
    }
    for (unsigned i = 0; i < ((8 - s.x87.fxsave32.swd.top) & 7); ++i) {
        out += std::format(" st{}={:.17g}", i, std::bit_cast<double>(st_bits(s, i)));
    }
    out += "\n  [ecx]";
    for (unsigned i = 0; i < 24; ++i) {
        out += std::format(" {:.9g}", mem::read<float>(ecx(s) + 4 * i));
    }
    return out + "\n";
}

float random_float(std::mt19937_64& rng) {
    static constexpr float kSpecial[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1e-3f, 1e5f, 3.14159265f};
    if (rng() % 16 == 0) {
        return kSpecial[rng() % std::size(kSpecial)];
    }
    return std::uniform_real_distribution<float>(-200.0f, 200.0f)(rng);
}

Case make_case(const GuestState& base, uint32_t scratch, unsigned iteration, std::mt19937_64& rng,
               const DiffTestOptions& options) {
    for (uint32_t off = 0; off < kBufferCount * kBufferSize; off += 4) {
        mem::write<float>(scratch + off, random_float(rng));
    }
    auto buffer = [&](unsigned k) { return scratch + k * kBufferSize + kBufferSize / 2; };
    Case c{};
    c.state = base;
    GuestState& s = c.state;
    eax(s) = buffer(0);
    ecx(s) = buffer(1);
    edx(s) = buffer(2);
    ebx(s) = buffer(3);
    ebp(s) = buffer(4);
    esi(s) = buffer(5);
    edi(s) = buffer(6);
    uint32_t sp = scratch + kStackTop - 4 * kArgSlots;
    unsigned pattern = iteration % 3;
    for (unsigned i = 0; i < kArgSlots; ++i) {
        bool pointer = pattern == 0 || (pattern == 2 && i % 2 == 0);
        uint32_t value = pointer ? buffer(7 + i) : std::bit_cast<uint32_t>(random_float(rng));
        mem::write<uint32_t>(sp + 4 * i, value);
    }
    sp -= 4;
    mem::write<uint32_t>(sp, kReturnSentinel);
    esp(s) = sp;
    eip(s) = 0;
    c.entry_esp = sp;
    std::memset(&s.st, 0, sizeof(s.st));
    s.x87.fxsave32.swd.top = 0;
    s.x87.fxsave32.cwd.flat = kControlWords[(iteration / 3) % kControlWords.size()];
    for (uint32_t i = 0; i < options.x87_args; ++i) {
        static constexpr double kEdges[] = {1.0, -1.0, 0.0, 0.5, -0.5};
        double v = rng() % 3 == 0 ? kEdges[rng() % std::size(kEdges)]
                                  : std::uniform_real_distribution<double>(-1.25, 1.25)(rng);
        x87_push(s, v);
    }
    c.deep = options.deep;
    return c;
}

std::vector<uint32_t> read_list(const std::filesystem::path& path) {
    std::vector<uint32_t> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        out.push_back(static_cast<uint32_t>(std::stoul(line, nullptr, 0)));
    }
    return out;
}

}

int run_diff_tests(const pe::Image& image, const DiffTestOptions& options) {
    auto targets = read_list(options.list);
    if (targets.empty()) {
        std::fprintf(stderr, "diff-test: no addresses in %s\n", options.list.c_str());
        return 1;
    }
    uint32_t scratch = allocator().alloc(kScratchSize);
    std::vector<Region> regions;
    for (const auto& sec : image.sections) {
        if (sec.writable() && !sec.executable()) {
            regions.push_back({image.image_base + sec.virtual_address, sec.mapped_size()});
        }
    }
    regions.push_back({scratch, kBufferCount * kBufferSize});
    GuestState& s = current_thread()->state;
    const GuestState base = s;
    unsigned agree = 0, mismatched = 0, inconclusive = 0, skipped = 0;
    for (uint32_t target : targets) {
        if (!lookup_trace(target)) {
            ++skipped;
            continue;
        }
        std::mt19937_64 rng(options.seed ^ (static_cast<uint64_t>(target) * 0x9e3779b97f4a7c15ull));
        bool clean = true;
        bool any_ok = false;
        for (uint32_t it = 0; it < options.iterations; ++it) {
            Case c = make_case(base, scratch, it, rng, options);
            auto with_stack = regions;
            with_stack.push_back({c.entry_esp + 4, scratch + kStackTop - c.entry_esp - 4});
            Report lifted = run_isolated(Mode::lifted, target, c, s, with_stack);
            Report interp = run_isolated(Mode::interpreted, target, c, s, with_stack);
            if (timed_out(lifted) || timed_out(interp) ||
                (lifted.status != Status::ok && interp.status != Status::ok)) {
                continue;
            }
            std::string diff = compare(lifted, interp);
            if (!diff.empty()) {
                std::fprintf(stderr, "[DIFF] 0x%08x iteration %u cw=%04x: %s\n", target, it,
                             static_cast<unsigned>(c.state.x87.fxsave32.cwd.flat), diff.c_str());
                if (options.verbose) {
                    std::fprintf(stderr, "%s", describe_case(c).c_str());
                    std::fprintf(stderr, "  lifted %s\n  interp %s\n", describe(lifted).c_str(),
                                 describe(interp).c_str());
                }
                clean = false;
                break;
            }
            any_ok = any_ok || lifted.status == Status::ok;
        }
        if (!clean) {
            ++mismatched;
        } else if (!any_ok) {
            ++inconclusive;
        } else {
            ++agree;
        }
    }
    std::fprintf(stderr,
                 "diff-test: %zu functions, %u iterations each: %u agree, %u differ, %u inconclusive, %u not lifted\n",
                 targets.size(), options.iterations, agree, mismatched, inconclusive, skipped);
    return mismatched == 0 ? 0 : 1;
}

}
