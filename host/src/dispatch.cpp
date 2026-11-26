#include "dispatch.hpp"

#include "win32/seh.hpp"

#include "macandreas/interp.hpp"

#include "crash.hpp"
#include "guest_memory.hpp"
#include "thunks.hpp"

#include <dlfcn.h>
#include <mach/mach_time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <vector>

namespace ma::host {

namespace {

struct FuncEntry {
    uint32_t addr;
    TraceFn fn;
};

class TraceTable {
public:
    void reserve(size_t count) {
        size_t capacity = 1024;
        while (capacity < count * 2) {
            capacity <<= 1;
        }
        if (capacity > slots_.size()) {
            auto old = std::move(slots_);
            slots_.assign(capacity, Slot{});
            mask_ = static_cast<uint32_t>(capacity - 1);
            size_ = 0;
            for (const auto& slot : old) {
                if (slot.addr != 0) {
                    insert(slot.addr, slot.fn);
                }
            }
        }
    }

    void insert(uint32_t addr, TraceFn fn) {
        if ((size_ + 1) * 2 > slots_.size()) {
            reserve(size_ + 1);
        }
        for (uint32_t i = hash(addr);; i = (i + 1) & mask_) {
            if (slots_[i].addr == addr) {
                slots_[i].fn = fn;
                return;
            }
            if (slots_[i].addr == 0) {
                slots_[i] = {addr, fn};
                ++size_;
                return;
            }
        }
    }

    size_t clear_range(uint32_t low, uint32_t high) {
        size_t n = 0;
        for (auto& slot : slots_) {
            if (slot.addr >= low && slot.addr < high && slot.fn) {
                slot.fn = nullptr;
                ++n;
            }
        }
        return n;
    }

    TraceFn find(uint32_t addr) const {
        if (slots_.empty() || addr == 0) {
            return nullptr;
        }
        for (uint32_t i = hash(addr);; i = (i + 1) & mask_) {
            if (slots_[i].addr == addr) {
                return slots_[i].fn;
            }
            if (slots_[i].addr == 0) {
                return nullptr;
            }
        }
    }

    size_t size() const { return size_; }

    std::vector<std::pair<uint32_t, TraceFn>> in_range(uint32_t low, uint32_t high) const {
        std::vector<std::pair<uint32_t, TraceFn>> out;
        for (const auto& slot : slots_) {
            if (slot.addr >= low && slot.addr < high && slot.fn) {
                out.emplace_back(slot.addr, slot.fn);
            }
        }
        return out;
    }

private:
    struct Slot {
        uint32_t addr = 0;
        TraceFn fn = nullptr;
    };

    uint32_t hash(uint32_t addr) const { return (addr * 0x9e3779b1u) >> 7 & mask_; }

    std::vector<Slot> slots_;
    uint32_t mask_ = 0;
    size_t size_ = 0;
};

TraceTable g_table;

uint64_t rdtsc() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    unsigned __int128 ns = static_cast<unsigned __int128>(mach_absolute_time()) * tb.numer / tb.denom;
    return static_cast<uint64_t>(ns * 2);
}

void cpuid(GuestState& s) {
    uint32_t leaf = eax(s);
    switch (leaf) {
        case 0:
            eax(s) = 1;
            ebx(s) = 0x756e6547;
            edx(s) = 0x49656e69;
            ecx(s) = 0x6c65746e;
            break;
        case 1:
            eax(s) = 0x000006f6;
            ebx(s) = 0x00010800;
            ecx(s) = 0;
            edx(s) = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
            break;
        case 0x80000000:
            eax(s) = 0x80000000;
            ebx(s) = ecx(s) = edx(s) = 0;
            break;
        default:
            eax(s) = ebx(s) = ecx(s) = edx(s) = 0;
            break;
    }
}

}

std::expected<size_t, std::string> load_function_table(void* dylib) {
    auto* table = static_cast<const FuncEntry*>(dlsym(dylib, "ma_functable"));
    auto* count = static_cast<const uint32_t*>(dlsym(dylib, "ma_functable_count"));
    if (!table || !count) {
        return std::unexpected("lifted dylib has no ma_functable");
    }
    g_table.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
        g_table.insert(table[i].addr, table[i].fn);
    }
    return g_table.size();
}

void register_trace(uint32_t addr, TraceFn fn) { g_table.insert(addr, fn); }

void force_interpret(uint32_t addr) { g_table.insert(addr, nullptr); }

size_t force_interpret_range(uint32_t low, uint32_t high) { return g_table.clear_range(low, high); }

TraceFn lookup_trace(uint32_t addr) { return g_table.find(addr); }

std::vector<std::pair<uint32_t, TraceFn>> traces_in_range(uint32_t low, uint32_t high) {
    return g_table.in_range(low, high);
}

void guest_fatal(GuestState& s, const char* kind, uint32_t addr) {
    std::fprintf(stderr, "[%s] 0x%08x (esp=0x%08x, return=0x%08x)\n", kind, addr, esp(s),
                 mem::read<uint32_t>(esp(s)));
    print_host_backtrace();
    std::fflush(stderr);
    std::_Exit(4);
}

namespace {

std::mutex g_interp_mutex;
std::unordered_set<uint32_t> g_interp_seen;
std::atomic<bool> g_interp_trace = false;

std::unordered_map<uint32_t, uint32_t> g_trace_pcs;

double trace_st(const GuestState& s, unsigned i) {
    double v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(&s.st.elems[i]) + 6, sizeof(v));
    return v;
}

void trace_pc(GuestState& s, uint32_t pc) {
    auto it = g_trace_pcs.find(pc);
    if (it == g_trace_pcs.end() || it->second >= 64) {
        return;
    }
    ++it->second;
    std::fprintf(stderr,
                 "[TRACE] %08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x "
                 "st0=%.9g st1=%.9g st2=%.9g [esp+32]=%.9g fcw=%04x\n",
                 pc, eax(s), ebx(s), ecx(s), edx(s), esi(s), edi(s), ebp(s), esp(s), trace_st(s, 0), trace_st(s, 1),
                 trace_st(s, 2), static_cast<double>(mem::read<float>(esp(s) + 32)),
                 static_cast<unsigned>(s.x87.fxsave32.cwd.flat));
}

bool interp_has_native(void* context, uint32_t pc) {
    if (!g_trace_pcs.empty() && context) {
        trace_pc(*static_cast<GuestState*>(context), pc);
    }
    return is_thunk(pc) || lookup_trace(pc) != nullptr;
}

void interp_call_native(void*, GuestState& s, uint32_t target) { dispatch(s, target); }

void interpret(GuestState& s, uint32_t target) {
    uint32_t caller = mem::read<uint32_t>(esp(s));
    {
        std::lock_guard lock(g_interp_mutex);
        if (g_interp_seen.insert(target).second || g_interp_trace) {
            std::fprintf(stderr, "[INTERP] 0x%08x caller=0x%08x\n", target, caller);
        }
    }
    interp::Hooks hooks{&s, interp_has_native, interp_call_native};
    auto result = interp::run(s, target, hooks);
    if (result.stop == interp::Stop::unsupported) {
        std::fprintf(stderr, "[INTERP] %s\n", result.message.c_str());
        guest_fatal(s, "INTERP", result.pc);
    }
}

}

void set_trace_interp(bool enabled) { g_interp_trace = enabled; }

void trace_interpreted_pc(uint32_t addr) { g_trace_pcs[addr] = 0; }

bool emulate_segment_instruction(GuestState& s) {
    constexpr uint32_t kCodeSelector = 0x1b;
    constexpr uint32_t kDataSelector = 0x23;
    uint8_t op = mem::read<uint8_t>(eip(s));
    switch (op) {
    case 0x06:
    case 0x16:
    case 0x1e:
        esp(s) -= 4;
        mem::write<uint32_t>(esp(s), kDataSelector);
        return true;
    case 0x0e:
        esp(s) -= 4;
        mem::write<uint32_t>(esp(s), kCodeSelector);
        return true;
    case 0x07:
    case 0x17:
    case 0x1f:
        esp(s) += 4;
        return true;
    default:
        return false;
    }
}

void finish_call(GuestState& s, uint32_t expected) {
    static std::mutex reported_mutex;
    static std::unordered_set<uint64_t> reported;
    while (eip(s) != expected) {
        uint32_t resume = eip(s);
        {
            std::lock_guard lock(reported_mutex);
            if (reported.insert((static_cast<uint64_t>(expected) << 32) | resume).second) {
                std::fprintf(stderr, "[RET] return to 0x%08x while 0x%08x was expected, continuing there\n", resume,
                             expected);
            }
        }
        dispatch(s, resume);
    }
}

namespace {

thread_local bool t_pending_jump = false;

}

void dispatch(GuestState& s, uint32_t target) {
    for (;;) {
        if (is_thunk(target)) {
            invoke_thunk(s, target);
        } else if (TraceFn fn = lookup_trace(target)) {
            eip(s) = target;
            fn(s, target, nullptr);
        } else {
            interpret(s, target);
        }
        if (!t_pending_jump) {
            return;
        }
        t_pending_jump = false;
        target = eip(s);
    }
}

void request_jump(GuestState& s, uint32_t target) {
    eip(s) = target;
    t_pending_jump = true;
}

void interpret_missing(GuestState& s, uint32_t target) { interpret(s, target); }

uint32_t call_guest(GuestState& s, uint32_t target, std::span<const uint32_t> args, bool callee_pops) {
    uint32_t saved_esp = esp(s);
    uint32_t saved_eip = eip(s);
    const uint32_t preserved[] = {ebx(s), esi(s), edi(s), ebp(s)};
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        esp(s) -= 4;
        mem::write<uint32_t>(esp(s), *it);
    }
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), kReturnSentinel);
    dispatch(s, target);
    finish_call(s, kReturnSentinel);
    uint32_t expected = saved_esp - (callee_pops ? 0u : 4u * static_cast<uint32_t>(args.size()));
    if (esp(s) != expected) {
        std::fprintf(stderr, "[ABI] call to 0x%08x left esp=0x%08x, expected 0x%08x\n", target, esp(s), expected);
    }
    const uint32_t after[] = {ebx(s), esi(s), edi(s), ebp(s)};
    static constexpr const char* kNames[] = {"ebx", "esi", "edi", "ebp"};
    for (int i = 0; i < 4; ++i) {
        if (after[i] != preserved[i]) {
            std::fprintf(stderr, "[ABI] call to 0x%08x changed %s 0x%08x -> 0x%08x\n", target, kNames[i], preserved[i],
                         after[i]);
        }
    }
    esp(s) = saved_esp;
    eip(s) = saved_eip;
    return eax(s);
}

uint32_t call_guest(GuestState& s, uint32_t target, std::initializer_list<uint32_t> args, bool callee_pops) {
    return call_guest(s, target, std::span<const uint32_t>(args.begin(), args.size()), callee_pops);
}

}

using ma::host::GuestState;

extern "C" {

__attribute__((visibility("default"))) Memory* __remill_function_call(GuestState& s, uint32_t pc, Memory* m) {
    uint32_t slot = ma::host::esp(s);
    uint32_t return_address = ma::host::mem::read<uint32_t>(slot);
    try {
        ma::host::dispatch(s, pc);
        ma::host::finish_call(s, return_address);
    } catch (const ma::host::win32::GuestUnwind& unwind) {
        if (!ma::host::win32::unwind_matches_call(unwind, slot, return_address)) {
            throw;
        }
        ma::host::esp(s) = unwind.esp;
        ma::host::eip(s) = unwind.eip;
    }
    return m;
}

__attribute__((visibility("default"))) Memory* __remill_jump(GuestState& s, uint32_t pc, Memory* m) {
    ma::host::request_jump(s, pc);
    return m;
}

__attribute__((visibility("default"))) Memory* __remill_missing_block(GuestState& s, uint32_t pc, Memory* m) {
    ma::host::interpret_missing(s, pc);
    return m;
}

__attribute__((visibility("default"))) Memory* __remill_error(GuestState& s, uint32_t pc, Memory* m) {
    ma::host::eip(s) = pc;
    if (ma::host::emulate_segment_instruction(s)) {
        ma::host::request_jump(s, pc + 1);
        return m;
    }
    ma::host::interpret_missing(s, pc);
    return m;
}

__attribute__((visibility("default"))) Memory* __remill_async_hyper_call(GuestState& s, uint32_t pc, Memory*) {
    ma::host::guest_fatal(s, "HYPERCALL", pc);
}

__attribute__((visibility("default"))) Memory* __remill_sync_hyper_call(GuestState& s, Memory* m,
                                                                        SyncHyperCall::Name call) {
    switch (call) {
        case SyncHyperCall::kX86CPUID:
            ma::host::cpuid(s);
            break;
        case SyncHyperCall::kX86ReadTSC: {
            uint64_t t = ma::host::rdtsc();
            ma::host::eax(s) = static_cast<uint32_t>(t);
            ma::host::edx(s) = static_cast<uint32_t>(t >> 32);
            break;
        }
        case SyncHyperCall::kX86SetSegmentES:
        case SyncHyperCall::kX86SetSegmentSS:
        case SyncHyperCall::kX86SetSegmentDS:
        case SyncHyperCall::kX86SetSegmentFS:
        case SyncHyperCall::kX86SetSegmentGS:
            break;
        case SyncHyperCall::kX86EmulateInstruction:
            if (!ma::host::emulate_segment_instruction(s)) {
                std::fprintf(stderr, "[HYPERCALL] cannot emulate instruction %02x at 0x%08x\n",
                             ma::host::mem::read<uint8_t>(ma::host::eip(s)), ma::host::eip(s));
                ma::host::guest_fatal(s, "HYPERCALL", ma::host::eip(s));
            }
            break;
        default:
            std::fprintf(stderr, "[HYPERCALL] unsupported sync hyper call %u\n", static_cast<unsigned>(call));
            ma::host::guest_fatal(s, "HYPERCALL", ma::host::eip(s));
    }
    return m;
}
}
