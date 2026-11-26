#include "shadow.hpp"

#include "abi.hpp"
#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "thread.hpp"
#include "thunks.hpp"

#include "macandreas/interp.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ma::host::shadow {

namespace {

constexpr uint32_t kTrackLow = 0x00010000;
constexpr uint32_t kTrackHigh = 0xf0000000;
constexpr size_t kMaxPages = 2048;
constexpr unsigned kTimeoutSeconds = 10;

enum class Status : uint32_t { ok, unsupported, overflow, exception, bad_return };

struct Header {
    Status status = Status::ok;
    uint32_t detail = 0;
    std::array<uint32_t, 8> gpr{};
    uint32_t eip = 0;
    uint32_t top = 0;
    std::array<uint64_t, 8> st{};
    uint32_t pages = 0;
};

struct Outcome {
    bool received = false;
    int signal = 0;
    int exit_code = 0;
    Header header;
    std::map<uint32_t, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> pages;
};

struct Stats {
    uint64_t calls = 0;
    uint32_t samples = 0;
    uint32_t agree = 0;
    uint32_t differ = 0;
    uint32_t inconclusive = 0;
};

enum class Mode { lifted, interpreted };

Options g_options;
std::unordered_map<uint32_t, TraceFn> g_orig;
std::map<uint32_t, Stats> g_stats;
GuestThread* g_main = nullptr;
std::atomic<bool> g_armed = false;
thread_local bool t_child = false;

size_t g_page_size = 0;
uint8_t* g_orig_copies = nullptr;
uintptr_t g_dirty[kMaxPages];
size_t g_dirty_count = 0;
bool g_overflow = false;

void on_fault(int sig, siginfo_t* info, void*) {
    auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
    auto low = reinterpret_cast<uintptr_t>(ma_guest_base) + kTrackLow;
    auto high = reinterpret_cast<uintptr_t>(ma_guest_base) + kTrackHigh;
    if (addr < low || addr >= high) {
        signal(sig, SIG_DFL);
        return;
    }
    uintptr_t page = addr & ~(g_page_size - 1);
    if (g_dirty_count < kMaxPages) {
        std::memcpy(g_orig_copies + g_dirty_count * g_page_size, reinterpret_cast<void*>(page), g_page_size);
        g_dirty[g_dirty_count++] = page;
    } else {
        g_overflow = true;
    }
    mprotect(reinterpret_cast<void*>(page), g_page_size, PROT_READ | PROT_WRITE);
}

uint64_t st_bits(const GuestState& s, unsigned i) {
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(&s.st.elems[i]) + 6, sizeof(v));
    return v;
}

bool interp_has_native(void*, uint32_t pc) { return is_thunk(pc); }

bool child_may_call(std::string_view dll, std::string_view name) {
    static constexpr std::string_view kDeniedDlls[] = {"d3d9", "dsound", "dinput8", "ddraw", "DSOUND", "DINPUT8",
                                                       "DDRAW", "d3d9.dll"};
    for (auto d : kDeniedDlls) {
        if (dll.starts_with(d)) {
            return false;
        }
    }
    static constexpr std::string_view kDenied[] = {"File", "Thread", "Wait", "Directory", "Event", "Semaphore",
                                                   "Mutex", "Sleep", "Message", "Window", "Cursor"};
    for (auto d : kDenied) {
        if (name.find(d) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

void interp_call_shallow(void*, GuestState& s, uint32_t target) { dispatch(s, target); }

void interp_call_deep(void* context, GuestState& s, uint32_t target) {
    if (is_thunk(target)) {
        dispatch(s, target);
        return;
    }
    interp::Hooks hooks{context, interp_has_native, interp_call_deep};
    auto result = interp::run(s, target, hooks);
    if (result.stop == interp::Stop::unsupported) {
        throw result;
    }
}

bool write_all(int fd, const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0) {
        ssize_t n = read(fd, p, size);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

[[noreturn]] void child(Mode mode, const GuestState& entry, uint32_t pc, int fd) {
    t_child = true;
    struct sigaction sa {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    for (int sig : {SIGILL, SIGFPE, SIGTRAP, SIGABRT}) {
        signal(sig, SIG_DFL);
    }
    alarm(kTimeoutSeconds);
    set_thunk_guard(child_may_call);
    mprotect(ma_guest_base + kTrackLow, kTrackHigh - kTrackLow, PROT_READ);
    GuestState s = entry;
    const uint32_t expected = mem::read<uint32_t>(esp(s));
    Header h;
    try {
        uint32_t target = pc;
        for (unsigned hop = 0; hop < 8 && h.status == Status::ok; ++hop) {
            if (mode == Mode::lifted) {
                dispatch(s, target);
            } else {
                interp::Hooks hooks{nullptr, interp_has_native,
                                    g_options.deep ? interp_call_deep : interp_call_shallow};
                auto result = interp::run(s, target, hooks);
                if (result.stop == interp::Stop::unsupported) {
                    h.status = Status::unsupported;
                    h.detail = result.pc;
                }
            }
            if (eip(s) == expected) {
                break;
            }
            target = eip(s);
        }
        if (h.status == Status::ok && eip(s) != expected) {
            h.status = Status::bad_return;
            h.detail = eip(s);
        }
    } catch (const interp::Result& r) {
        h.status = Status::unsupported;
        h.detail = r.pc;
    } catch (...) {
        h.status = Status::exception;
    }
    mprotect(ma_guest_base + kTrackLow, kTrackHigh - kTrackLow, PROT_READ | PROT_WRITE);
    if (g_overflow && h.status == Status::ok) {
        h.status = Status::overflow;
    }
    h.gpr = {eax(s), ecx(s), edx(s), ebx(s), esp(s), ebp(s), esi(s), edi(s)};
    h.eip = eip(s);
    h.top = s.x87.fxsave32.swd.top;
    unsigned depth = (8 - h.top) & 7;
    for (unsigned i = 0; i < depth; ++i) {
        h.st[i] = st_bits(s, i);
    }
    h.pages = static_cast<uint32_t>(g_dirty_count);
    bool ok = write_all(fd, &h, sizeof(h));
    for (size_t i = 0; ok && i < g_dirty_count; ++i) {
        uint32_t guest = static_cast<uint32_t>(g_dirty[i] - reinterpret_cast<uintptr_t>(ma_guest_base));
        ok = write_all(fd, &guest, sizeof(guest)) && write_all(fd, g_orig_copies + i * g_page_size, g_page_size) &&
             write_all(fd, reinterpret_cast<void*>(g_dirty[i]), g_page_size);
    }
    _exit(ok ? 0 : 1);
}

Outcome run(Mode mode, const GuestState& entry, uint32_t pc) {
    Outcome out;
    int fds[2];
    if (pipe(fds) != 0) {
        return out;
    }
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        child(mode, entry, pc, fds[1]);
    }
    close(fds[1]);
    if (pid > 0 && read_all(fds[0], &out.header, sizeof(out.header))) {
        out.received = true;
        for (uint32_t i = 0; i < out.header.pages; ++i) {
            uint32_t guest = 0;
            std::vector<uint8_t> before(g_page_size), after(g_page_size);
            if (!read_all(fds[0], &guest, sizeof(guest)) || !read_all(fds[0], before.data(), g_page_size) ||
                !read_all(fds[0], after.data(), g_page_size)) {
                out.received = false;
                break;
            }
            out.pages[guest] = {std::move(before), std::move(after)};
        }
    }
    close(fds[0]);
    int status = 0;
    if (pid > 0) {
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            out.signal = WTERMSIG(status);
            out.received = false;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            out.exit_code = WEXITSTATUS(status);
            out.received = false;
        }
    }
    return out;
}

std::string status_text(const Outcome& o) {
    if (!o.received) {
        if (o.exit_code == 43) {
            return "called an import with side effects";
        }
        return o.signal ? std::format("signal {}", o.signal) : "no report";
    }
    switch (o.header.status) {
    case Status::ok: return "ok";
    case Status::unsupported: return std::format("unsupported at 0x{:08x}", o.header.detail);
    case Status::overflow: return "too many pages written";
    case Status::exception: return "exception";
    case Status::bad_return: return std::format("handed off at 0x{:08x}", o.header.detail);
    }
    return "?";
}

bool same_double(uint64_t a, uint64_t b) {
    return a == b || (std::isnan(std::bit_cast<double>(a)) && std::isnan(std::bit_cast<double>(b)));
}

const std::vector<uint8_t>* final_bytes(const Outcome& o, uint32_t page, const Outcome& other) {
    if (auto it = o.pages.find(page); it != o.pages.end()) {
        return &it->second.second;
    }
    return &other.pages.at(page).first;
}

std::string compare(const Outcome& lifted, const Outcome& interp) {
    static constexpr const char* kNames[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    std::string out;
    for (unsigned i = 0; i < 8; ++i) {
        if (lifted.header.gpr[i] != interp.header.gpr[i]) {
            out += std::format(" {} lifted=0x{:08x} interp=0x{:08x};", kNames[i], lifted.header.gpr[i],
                               interp.header.gpr[i]);
        }
    }
    if (lifted.header.eip != interp.header.eip) {
        out += std::format(" eip lifted=0x{:08x} interp=0x{:08x};", lifted.header.eip, interp.header.eip);
    }
    if (lifted.header.top != interp.header.top) {
        out += std::format(" x87 top lifted={} interp={};", lifted.header.top, interp.header.top);
    } else {
        for (unsigned i = 0; i < ((8 - lifted.header.top) & 7); ++i) {
            if (!same_double(lifted.header.st[i], interp.header.st[i])) {
                out += std::format(" st{} lifted={:.17g} interp={:.17g};", i,
                                   std::bit_cast<double>(lifted.header.st[i]),
                                   std::bit_cast<double>(interp.header.st[i]));
            }
        }
    }
    std::vector<uint32_t> pages;
    for (const auto& [p, _] : lifted.pages) {
        pages.push_back(p);
    }
    for (const auto& [p, _] : interp.pages) {
        if (!lifted.pages.contains(p)) {
            pages.push_back(p);
        }
    }
    std::ranges::sort(pages);
    unsigned shown = 0;
    unsigned total = 0;
    uint32_t final_esp = lifted.header.gpr[4];
    bool same_esp = final_esp == interp.header.gpr[4];
    for (uint32_t page : pages) {
        const auto* a = final_bytes(lifted, page, interp);
        const auto* b = final_bytes(interp, page, lifted);
        for (size_t off = 0; off + 4 <= g_page_size; off += 4) {
            if (std::memcmp(a->data() + off, b->data() + off, 4) == 0) {
                continue;
            }
            uint32_t addr = page + static_cast<uint32_t>(off);
            if (same_esp && addr < final_esp && addr >= final_esp - 0x10000) {
                continue;
            }
            ++total;
            if (shown++ < 8) {
                uint32_t va;
                uint32_t vb;
                std::memcpy(&va, a->data() + off, 4);
                std::memcpy(&vb, b->data() + off, 4);
                out += std::format(" [{:08x}] lifted={:08x}({:.9g}) interp={:08x}({:.9g});",
                                   page + static_cast<uint32_t>(off), va, std::bit_cast<float>(va), vb,
                                   std::bit_cast<float>(vb));
            }
        }
    }
    if (total > shown) {
        out += std::format(" {} differing dwords in all;", total);
    }
    return out;
}

std::string describe_entry(GuestState s) {
    std::string out = std::format("ecx={:08x} edx={:08x} args", ecx(s), edx(s));
    for (unsigned i = 1; i <= 6; ++i) {
        uint32_t v = mem::read<uint32_t>(esp(s) + 4 * i);
        out += std::format(" {:08x}", v);
    }
    out += std::format(" return={:08x}", mem::read<uint32_t>(esp(s)));
    return out;
}

void compare_call(GuestState& s, uint32_t pc, Stats& stats) {
    GuestState entry = s;
    Outcome lifted = run(Mode::lifted, entry, pc);
    Outcome interp = run(Mode::interpreted, entry, pc);
    bool lifted_ok = lifted.received && lifted.header.status == Status::ok;
    bool interp_ok = interp.received && interp.header.status == Status::ok;
    if (!lifted_ok || !interp_ok) {
        ++stats.inconclusive;
        std::fprintf(stderr, "[SHADOW] 0x%08x sample %u inconclusive: lifted %s, interp %s\n", pc, stats.samples,
                     status_text(lifted).c_str(), status_text(interp).c_str());
        return;
    }
    std::string diff = compare(lifted, interp);
    if (diff.empty()) {
        ++stats.agree;
        return;
    }
    ++stats.differ;
    std::fprintf(stderr, "[SHADOW] 0x%08x sample %u differs:%s\n  entry %s\n", pc, stats.samples, diff.c_str(),
                 describe_entry(entry).c_str());
}

Memory* shadow_trace(GuestState& s, uint32_t pc, Memory* m) {
    TraceFn orig = g_orig.at(pc);
    if (t_child || !g_armed.load(std::memory_order_relaxed) || current_thread() != g_main) {
        return orig(s, pc, m);
    }
    Stats& stats = g_stats[pc];
    uint64_t n = stats.calls++;
    if (stats.samples < g_options.samples && n % g_options.every == 0) {
        ++stats.samples;
        compare_call(s, pc, stats);
    }
    return orig(s, pc, m);
}

struct Probe {
    uint64_t calls = 0;
    uint64_t zero = 0;
};

std::map<uint32_t, Probe> g_probes;

Memory* probe_trace(GuestState& s, uint32_t pc, Memory* m) {
    TraceFn orig = g_orig.at(pc);
    if (t_child) {
        return orig(s, pc, m);
    }
    Probe& p = g_probes[pc];
    uint64_t n = p.calls++;
    std::string entry = n < 12 ? describe_entry(s) : std::string{};
    Memory* r = orig(s, pc, m);
    if (eax(s) == 0) {
        ++p.zero;
    }
    if (n < 12) {
        std::fprintf(stderr, "[PROBE] 0x%08x call %llu: %s -> eax=0x%08x\n", pc, static_cast<unsigned long long>(n),
                     entry.c_str(), eax(s));
    }
    return r;
}

std::map<uint32_t, uint32_t> g_touches;

Memory* touch_trace(GuestState& s, uint32_t pc, Memory* m) {
    TraceFn orig = g_orig.at(pc);
    if (t_child) {
        return orig(s, pc, m);
    }
    uint32_t value = g_options.touch_value;
    auto matches = [value](uint32_t v) {
        if (value >= 0x10000) {
            return v >= value && v < value + 0x400;
        }
        uint32_t pool = mem::read<uint32_t>(0xb7449c);
        if (!pool) {
            return false;
        }
        uint32_t base = mem::read<uint32_t>(pool);
        uint32_t size = mem::read<uint32_t>(pool + 8);
        if (v < base || v >= base + size * 0x19c || (v - base) % 0x19c != 0) {
            return false;
        }
        return static_cast<uint32_t>(mem::read<int16_t>(v + 0x22)) == value;
    };
    bool hit = matches(ecx(s)) || matches(edx(s)) || matches(esi(s)) || matches(edi(s));
    for (unsigned i = 1; i <= 6 && !hit; ++i) {
        hit = matches(mem::read<uint32_t>(esp(s) + 4 * i));
    }
    if (!hit || !g_armed.load(std::memory_order_relaxed) || g_touches[pc]++ >= 40) {
        return orig(s, pc, m);
    }
    std::string entry = describe_entry(s);
    for (unsigned a = 1; a <= 2; ++a) {
        uint32_t ptr = mem::read<uint32_t>(esp(s) + 4 * a);
        if (ptr >= kNullGuardEnd && ptr < kAllocEnd) {
            entry += std::format("\n    arg{} [{:08x}]", a, ptr);
            for (unsigned k = 0; k < 8; ++k) {
                entry += std::format(" {:.4f}", mem::read<float>(ptr + 4 * k));
            }
        }
    }
    Memory* r = orig(s, pc, m);
    std::fprintf(stderr, "[TOUCH] 0x%08x: %s -> eax=0x%08x\n", pc, entry.c_str(), eax(s));
    return r;
}

void report() {
    for (const auto& [pc, p] : g_probes) {
        std::fprintf(stderr, "[PROBE] 0x%08x: %llu calls, %llu returned 0\n", pc, static_cast<unsigned long long>(p.calls),
                     static_cast<unsigned long long>(p.zero));
    }
    if (g_stats.empty()) {
        return;
    }
    unsigned functions = 0;
    unsigned agree = 0;
    unsigned differ = 0;
    unsigned inconclusive = 0;
    for (const auto& [pc, st] : g_stats) {
        if (st.samples == 0) {
            continue;
        }
        ++functions;
        agree += st.agree;
        differ += st.differ;
        inconclusive += st.inconclusive;
    }
    std::fprintf(stderr, "[SHADOW] %u functions sampled: %u samples agree, %u differ, %u inconclusive\n", functions,
                 agree, differ, inconclusive);
    for (const auto& [pc, st] : g_stats) {
        if (st.differ != 0) {
            std::fprintf(stderr, "[SHADOW]   0x%08x differs in %u of %u samples (%llu calls)\n", pc, st.differ,
                         st.samples, static_cast<unsigned long long>(st.calls));
        }
    }
}

}

void install(const Options& options) {
    g_options = options;
    g_options.every = std::max(1u, g_options.every);
    g_page_size = static_cast<size_t>(getpagesize());
    g_orig_copies = static_cast<uint8_t*>(
        mmap(nullptr, kMaxPages * g_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    g_main = current_thread();
    size_t count = 0;
    for (auto [low, high] : options.ranges) {
        for (auto [addr, fn] : traces_in_range(low, high)) {
            if (g_orig.emplace(addr, fn).second) {
                register_trace(addr, shadow_trace);
                ++count;
            }
        }
    }
    if (count != 0) {
        std::fprintf(stderr, "[SHADOW] watching %zu functions from frame %llu, %u samples each%s\n", count,
                     static_cast<unsigned long long>(options.from_frame), g_options.samples,
                     options.deep ? ", callees interpreted" : "");
    }
    size_t touched = 0;
    for (auto [low, high] : options.touch_ranges) {
        for (auto [addr, fn] : traces_in_range(low, high)) {
            if (g_orig.emplace(addr, fn).second) {
                register_trace(addr, touch_trace);
                ++touched;
            }
        }
    }
    if (touched != 0) {
        std::fprintf(stderr, "[TOUCH] watching %zu functions for 0x%08x\n", touched, options.touch_value);
    }
    for (uint32_t addr : options.probes) {
        TraceFn fn = lookup_trace(addr);
        if (!fn || g_orig.contains(addr)) {
            std::fprintf(stderr, "[PROBE] 0x%08x is not a lifted function\n", addr);
            continue;
        }
        g_orig.emplace(addr, fn);
        g_probes[addr];
        register_trace(addr, probe_trace);
    }
}

void note_frame(uint64_t frame) {
    if (g_orig.empty()) {
        return;
    }
    if (!g_armed && frame >= g_options.from_frame) {
        g_armed = true;
    }
    if ((g_armed || !g_probes.empty()) && frame % 300 == 0) {
        report();
    }
}

}
