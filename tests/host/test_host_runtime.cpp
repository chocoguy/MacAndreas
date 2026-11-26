#include "check.hpp"

#include "abi.hpp"
#include "audio/mix.hpp"
#include "audio/stream.hpp"
#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "input/input_state.hpp"
#include "input/keys.hpp"
#include "perf.hpp"
#include "platform/viewport.hpp"
#include "thread.hpp"
#include "settings.hpp"
#include "thunks.hpp"
#include "win32/process.hpp"
#include "win32/seh.hpp"
#include "win32/user32.hpp"
#include "win32/wsprintf.hpp"

#include "macandreas/interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ma::host;

extern "C" Memory* __remill_function_call(GuestState& s, uint32_t pc, Memory* m);

namespace {

uint32_t pop32(GuestState& s) {
    uint32_t v = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
    return v;
}

void push32(GuestState& s, uint32_t v) {
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), v);
}

void guest_ret(GuestState& s, uint32_t pop_bytes) {
    eip(s) = pop32(s);
    esp(s) += pop_bytes;
}

void guest_call_insn(GuestState& s, uint32_t return_address, uint32_t target) {
    push32(s, return_address);
    __remill_function_call(s, target, nullptr);
}

uint32_t stack_arg(GuestState& s, unsigned i) { return mem::read<uint32_t>(esp(s) + 4 + 4 * i); }

int32_t add3(int32_t a, int32_t b, int32_t c) { return a + b + c; }
int64_t mul64(int64_t a, int32_t b) { return a * b; }
uint32_t member_add(uint32_t self, uint32_t x, uint32_t y) { return self + x * y; }
float scale(float v, double k) { return static_cast<float>(v * k); }
double average(double a, double b) { return (a + b) / 2; }

int g_native_calls = 0;
uint32_t note_call(uint32_t v) {
    ++g_native_calls;
    return v * 10;
}

constexpr uint32_t kGuestAddTwice = 0x00401000;
constexpr uint32_t kGuestReenter = 0x00401100;
constexpr uint32_t kGuestE = 0x00402000;
constexpr uint32_t kGuestF = 0x00402100;
constexpr uint32_t kGuestG = 0x00402200;
constexpr uint32_t kHandlerSearch = 0x00403000;
constexpr uint32_t kHandlerContinue = 0x00403100;
constexpr uint32_t kHandlerExcept = 0x00403200;
constexpr uint32_t kEReturn = 0x00402050;
constexpr uint32_t kFReturn = 0x00402150;
constexpr uint32_t kGReturn = 0x00402250;
constexpr uint32_t kHReturn = 0x00403250;

struct EnumSeen {
    uint32_t devtype = 0;
    std::array<uint8_t, 16> guid{};
};
std::vector<EnumSeen> g_enumerated;

uint32_t enum_cb(uint32_t inst, uint32_t ref) {
    EnumSeen e;
    e.devtype = mem::read<uint32_t>(inst + 36);
    std::memcpy(e.guid.data(), mem::ptr(inst + 4), 16);
    g_enumerated.push_back(e);
    return 1;
}

constexpr ApiDef kTestApis[] = {
    native_export<Conv::stdcall, add3>("TEST.dll", "add3"),
    native_export<Conv::cdecl_, mul64>("TEST.dll", "mul64"),
    native_export<Conv::thiscall, member_add>("TEST.dll", "member_add"),
    native_export<Conv::stdcall, scale>("TEST.dll", "scale"),
    native_export<Conv::cdecl_, average>("TEST.dll", "average"),
    native_export<Conv::stdcall, note_call>("TEST.dll", "note_call"),
    native_export<Conv::stdcall, enum_cb>("TEST.dll", "enum_cb"),
};

uint32_t api(std::string_view name) { return thunk_for("TEST.dll", name, 0); }

Memory* guest_add_twice(GuestState& s, uint32_t, Memory* m) {
    uint32_t a = stack_arg(s, 0);
    uint32_t b = stack_arg(s, 1);
    uint32_t saved_esp = esp(s);
    push32(s, a);
    guest_call_insn(s, kGuestAddTwice + 0x20, api("note_call"));
    CHECK(eip(s) == kGuestAddTwice + 0x20);
    CHECK(esp(s) == saved_esp);
    uint32_t first = eax(s);
    push32(s, b);
    guest_call_insn(s, kGuestAddTwice + 0x30, api("note_call"));
    eax(s) = first + eax(s);
    guest_ret(s, 8);
    return m;
}

int g_reenter_depth = 0;
uint32_t reenter_native(uint32_t depth);

constexpr ApiDef kReenterApi[] = {
    native_export<Conv::stdcall, reenter_native>("TEST.dll", "reenter_native"),
};

Memory* guest_reenter(GuestState& s, uint32_t, Memory* m) {
    uint32_t depth = stack_arg(s, 0);
    ++g_reenter_depth;
    if (depth == 0) {
        eax(s) = 1;
    } else {
        push32(s, depth - 1);
        guest_call_insn(s, kGuestReenter + 0x10, api("reenter_native"));
        eax(s) += 1;
    }
    guest_ret(s, 4);
    return m;
}

uint32_t reenter_native(uint32_t depth) {
    GuestState& s = current_thread()->state;
    return guest_call<Conv::stdcall, uint32_t>(s, kGuestReenter, depth);
}

struct HandlerLog {
    std::vector<uint32_t> handlers;
    std::vector<uint32_t> frames;
    std::vector<uint32_t> flags;
    uint32_t code = 0;
};
HandlerLog g_log;

void log_handler(GuestState& s, uint32_t handler) {
    uint32_t record = stack_arg(s, 0);
    g_log.handlers.push_back(handler);
    g_log.frames.push_back(stack_arg(s, 1));
    g_log.flags.push_back(mem::read<uint32_t>(record + 4));
    g_log.code = mem::read<uint32_t>(record);
}

Memory* handler_search(GuestState& s, uint32_t, Memory* m) {
    log_handler(s, kHandlerSearch);
    eax(s) = static_cast<uint32_t>(win32::Disposition::continue_search);
    guest_ret(s, 0);
    return m;
}

Memory* handler_continue(GuestState& s, uint32_t, Memory* m) {
    log_handler(s, kHandlerContinue);
    uint32_t context = stack_arg(s, 2);
    if (!(mem::read<uint32_t>(stack_arg(s, 0) + 4) & win32::kExceptionUnwinding)) {
        mem::write<uint32_t>(context + win32::context_offset::eax, 0x1234);
    }
    eax(s) = static_cast<uint32_t>(win32::Disposition::continue_execution);
    guest_ret(s, 0);
    return m;
}

uint32_t g_f_registration = 0;
uint32_t g_f_return_slot = 0;
bool g_f_resumed_normally = false;
bool g_e_resumed = false;

Memory* handler_except(GuestState& s, uint32_t, Memory* m) {
    log_handler(s, kHandlerExcept);
    uint32_t record = stack_arg(s, 0);
    uint32_t frame = stack_arg(s, 1);
    if (mem::read<uint32_t>(record + 4) & win32::kExceptionUnwinding) {
        eax(s) = static_cast<uint32_t>(win32::Disposition::continue_search);
        guest_ret(s, 0);
        return m;
    }
    push32(s, 0);
    push32(s, record);
    push32(s, kHReturn);
    push32(s, frame);
    guest_call_insn(s, kHReturn, thunk_for("KERNEL32.dll", "RtlUnwind", 0));
    win32::set_seh_head(s, mem::read<uint32_t>(frame));
    esp(s) = g_f_return_slot;
    eax(s) = 0x77;
    guest_ret(s, 0);
    return m;
}

Memory* guest_g(GuestState& s, uint32_t, Memory* m) {
    push32(s, 0);
    push32(s, 0);
    push32(s, 0);
    push32(s, 0xe0434f4d);
    guest_call_insn(s, kGReturn, thunk_for("KERNEL32.dll", "RaiseException", 0));
    eax(s) = 0xdead;
    guest_ret(s, 0);
    return m;
}

Memory* guest_f(GuestState& s, uint32_t, Memory* m) {
    g_f_return_slot = esp(s);
    push32(s, kHandlerExcept);
    push32(s, win32::seh_head(s));
    g_f_registration = esp(s);
    win32::set_seh_head(s, g_f_registration);
    guest_call_insn(s, kFReturn, kGuestG);
    g_f_resumed_normally = true;
    win32::set_seh_head(s, mem::read<uint32_t>(g_f_registration));
    esp(s) = g_f_return_slot;
    guest_ret(s, 0);
    return m;
}

Memory* guest_e(GuestState& s, uint32_t, Memory* m) {
    uint32_t before = esp(s);
    guest_call_insn(s, kEReturn, kGuestF);
    g_e_resumed = eip(s) == kEReturn && esp(s) == before;
    eax(s) += 1;
    guest_ret(s, 0);
    return m;
}

void test_calling_conventions(GuestState& s) {
    uint32_t before = esp(s);
    CHECK((guest_call<Conv::stdcall, int32_t>(s, api("add3"), 1, -2, 40)) == 39);
    CHECK(esp(s) == before);
    CHECK((guest_call<Conv::cdecl_, int64_t>(s, api("mul64"), int64_t{0x100000000}, 3)) == 0x300000000);
    CHECK(esp(s) == before);
    CHECK((guest_call<Conv::thiscall, uint32_t>(s, api("member_add"), 1000u, 6u, 7u)) == 1042);
    CHECK(esp(s) == before);
    float scaled = guest_call<Conv::stdcall, float>(s, api("scale"), 1.5f, 4.0);
    CHECK(scaled == 6.0f);
    CHECK(s.x87.fxsave32.swd.top == 0);
    double avg = guest_call<Conv::cdecl_, double>(s, api("average"), 1.0, 2.0);
    CHECK(avg == 1.5);
    CHECK(esp(s) == before);

    CHECK(kTestApis[0].args == 3);
    CHECK(kTestApis[1].args == 3);
    CHECK(kTestApis[2].args == 2);
    CHECK(kTestApis[3].args == 3);
    CHECK(kTestApis[2].conv == Conv::thiscall);
}

void test_native_guest_bridge(GuestState& s) {
    uint32_t before_esp = esp(s);
    uint32_t before_eip = eip(s);
    uint32_t before_ebx = ebx(s) = 0x0badf00d;
    g_native_calls = 0;
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, kGuestAddTwice, 5u, 7u)) == 120);
    CHECK(g_native_calls == 2);
    CHECK(esp(s) == before_esp);
    CHECK(eip(s) == before_eip);
    CHECK(ebx(s) == before_ebx);

    g_reenter_depth = 0;
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, kGuestReenter, 4u)) == 5);
    CHECK(g_reenter_depth == 5);
    CHECK(esp(s) == before_esp);
}

void test_seh_chain(GuestState& s) {
    uint32_t base = esp(s);
    uint32_t inner = base - 0x100;
    uint32_t outer = base - 0x80;
    mem::write<uint32_t>(outer, win32::seh_head(s));
    mem::write<uint32_t>(outer + 4, kHandlerContinue);
    mem::write<uint32_t>(inner, outer);
    mem::write<uint32_t>(inner + 4, kHandlerSearch);
    uint32_t original_head = win32::seh_head(s);
    win32::set_seh_head(s, inner);
    esp(s) = base - 0x200;

    g_log = {};
    uint32_t params[2] = {11, 22};
    uint32_t params_addr = base - 0x300;
    mem::write<uint32_t>(params_addr, params[0]);
    mem::write<uint32_t>(params_addr + 4, params[1]);
    uint32_t before = esp(s);
    guest_call<Conv::stdcall>(s, thunk_for("KERNEL32.dll", "RaiseException", 0), 0xe06d7363u, 0u, 2u, params_addr);
    CHECK(g_log.handlers.size() == 2);
    if (g_log.handlers.size() == 2) {
        CHECK(g_log.handlers[0] == kHandlerSearch);
        CHECK(g_log.handlers[1] == kHandlerContinue);
        CHECK(g_log.frames[0] == inner);
        CHECK(g_log.frames[1] == outer);
        CHECK((g_log.flags[0] & win32::kExceptionUnwinding) == 0);
    }
    CHECK(g_log.code == 0xe06d7363u);
    CHECK(eax(s) == 0x1234);
    CHECK(esp(s) == before);
    CHECK(win32::seh_head(s) == inner);

    g_log = {};
    uint32_t r = guest_call<Conv::stdcall, uint32_t>(s, thunk_for("KERNEL32.dll", "RtlUnwind", 0), outer, 0u, 0u,
                                                     0x55u);
    CHECK(r == 0x55);
    CHECK(win32::seh_head(s) == outer);
    CHECK(g_log.handlers.size() == 1);
    if (g_log.handlers.size() == 1) {
        CHECK(g_log.handlers[0] == kHandlerSearch);
        CHECK((g_log.flags[0] & win32::kExceptionUnwinding) != 0);
    }
    CHECK(esp(s) == before);

    win32::set_seh_head(s, original_head);
    esp(s) = base;
}

void test_seh_nonlocal_unwind(GuestState& s) {
    uint32_t original_head = win32::seh_head(s);
    uint32_t before = esp(s);
    g_log = {};
    g_f_resumed_normally = false;
    g_e_resumed = false;
    eax(s) = 0;
    uint32_t r = guest_call<Conv::cdecl_, uint32_t>(s, kGuestE);
    CHECK(r == 0x78);
    CHECK(g_e_resumed);
    CHECK(!g_f_resumed_normally);
    CHECK(win32::seh_head(s) == original_head);
    CHECK(esp(s) == before);
    CHECK(g_log.handlers.size() >= 1);
    if (!g_log.handlers.empty()) {
        CHECK(g_log.handlers[0] == kHandlerExcept);
        CHECK(g_log.frames[0] == g_f_registration);
    }

    win32::GuestUnwind u{0x00401234, 0x1000};
    CHECK(win32::unwind_matches_call(u, 0x0ffc, 0x00401234));
    CHECK(win32::unwind_matches_call(u, 0x0ff0, 0x00401234));
    CHECK(!win32::unwind_matches_call(u, 0x0ffc, 0x00401238));
    CHECK(!win32::unwind_matches_call(u, 0x1000, 0x00401234));
    CHECK(!win32::unwind_matches_call(u, 0x0e00, 0x00401234));
}

constexpr uint32_t kCodeSum = 0x00500000;
constexpr uint32_t kCodeHandoff = 0x00500100;
constexpr uint32_t kCodeCalls = 0x00500200;
constexpr uint32_t kCodeX87 = 0x00500300;
constexpr uint32_t kCodePrecision = 0x00500400;
constexpr uint32_t kCodeExchange = 0x00500500;
constexpr uint32_t kCodeArctan = 0x00500600;
constexpr uint32_t kCodeIat = 0x00500f00;
constexpr uint32_t kLiftedBlock = 0x00401200;
constexpr uint32_t kLiftedDouble = 0x00401300;
constexpr uint32_t kLiftedCallsInterp = 0x00401400;

void write_code(uint32_t addr, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) {
        mem::write<uint8_t>(addr++, b);
    }
}

void write_rel32(uint32_t at, uint32_t next, uint32_t target) { mem::write<uint32_t>(at, target - next); }

bool g_block_saw_zf = false;

Memory* lifted_block(GuestState& s, uint32_t, Memory* m) {
    g_block_saw_zf = s.aflag.zf != 0;
    eax(s) = s.aflag.zf ? 100 : 200;
    guest_ret(s, 0);
    return m;
}

Memory* lifted_double(GuestState& s, uint32_t, Memory* m) {
    eax(s) = stack_arg(s, 0) * 2;
    guest_ret(s, 4);
    return m;
}

Memory* lifted_calls_interp(GuestState& s, uint32_t, Memory* m) {
    push32(s, stack_arg(s, 0));
    guest_call_insn(s, kLiftedCallsInterp + 0x10, kCodeSum);
    CHECK(eip(s) == kLiftedCallsInterp + 0x10);
    esp(s) += 4;
    eax(s) += 1;
    guest_ret(s, 0);
    return m;
}

void install_interp_fixtures() {
    write_code(kCodeSum, {0x8b, 0x4c, 0x24, 0x04, 0x31, 0xc0, 0x85, 0xc9, 0x7e, 0x08, 0x8d, 0x14, 0x49, 0x01, 0xd0,
                          0x49, 0x75, 0xf8, 0xc3});
    write_code(kCodeHandoff, {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x05, 0xe9, 0, 0, 0, 0});
    write_rel32(kCodeHandoff + 8, kCodeHandoff + 12, kLiftedBlock);
    write_code(kCodeCalls, {0x8b, 0x44, 0x24, 0x04, 0x50, 0xe8, 0, 0, 0, 0, 0x50, 0xff, 0x15, 0, 0, 0, 0, 0xc2, 0x04,
                            0x00});
    write_rel32(kCodeCalls + 6, kCodeCalls + 10, kLiftedDouble);
    mem::write<uint32_t>(kCodeCalls + 13, kCodeIat);
    mem::write<uint32_t>(kCodeIat, api("note_call"));
    write_code(kCodeX87, {0x8b, 0x44, 0x24, 0x04, 0xd9, 0x00, 0xd8, 0x08, 0x83, 0xec, 0x04, 0xdb, 0x1c, 0x24, 0x58,
                          0xc3});
    write_code(kCodePrecision, {0x8b, 0x44, 0x24, 0x04, 0xd9, 0x00, 0xd8, 0x48, 0x04, 0xd9, 0xfa, 0xdd, 0x58, 0x08, 0xc3});
    write_code(kCodeExchange, {0x8b, 0x44, 0x24, 0x04, 0xd9, 0x00, 0xd9, 0x40, 0x04, 0xd9, 0x40, 0x08, 0xd9, 0x40, 0x0c,
                               0xd9, 0xcb, 0xd9, 0x58, 0x10, 0xdd, 0xd8, 0xdd, 0xd8, 0xdd, 0xd8, 0xc3});
    write_code(kCodeArctan, {0x8b, 0x44, 0x24, 0x04, 0xd9, 0x00, 0xd9, 0x40, 0x04, 0xd9, 0xf3, 0xdd, 0x58, 0x08, 0xc3});
    register_trace(kLiftedBlock, lifted_block);
    register_trace(kLiftedDouble, lifted_double);
    register_trace(kLiftedCallsInterp, lifted_calls_interp);
}

void test_interpreter(GuestState& s) {
    install_interp_fixtures();
    uint32_t before = esp(s);

    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kCodeSum, 4u)) == 30);
    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kCodeSum, 0u)) == 0);
    CHECK(esp(s) == before);

    g_block_saw_zf = false;
    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kCodeHandoff, 5u)) == 100);
    CHECK(g_block_saw_zf);
    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kCodeHandoff, 6u)) == 200);
    CHECK(esp(s) == before);

    g_native_calls = 0;
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, kCodeCalls, 21u)) == 420);
    CHECK(g_native_calls == 1);
    CHECK(esp(s) == before);

    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kLiftedCallsInterp, 3u)) == 19);
    CHECK(esp(s) == before);

    uint32_t value = before - 0x400;
    mem::write<float>(value, 2.5f);
    uint16_t top = s.x87.fxsave32.swd.top;
    CHECK((guest_call<Conv::cdecl_, uint32_t>(s, kCodeX87, value)) == 6);
    CHECK(s.x87.fxsave32.swd.top == top);
    CHECK(esp(s) == before);

    CHECK(ma::interp::describe(kCodeSum).starts_with("mov ecx, [esp+0x04]"));

    uint16_t saved_cw = s.x87.fxsave32.cwd.flat;
    mem::write<float>(value, 5474.0f);
    mem::write<float>(value + 4, 1.0f / 3.0f);
    double exact = std::sqrt(5474.0 * static_cast<double>(1.0f / 3.0f));
    s.x87.fxsave32.cwd.flat = 0x027f;
    guest_call<Conv::cdecl_, uint32_t>(s, kCodePrecision, value);
    CHECK(mem::read<double>(value + 8) == exact);
    s.x87.fxsave32.cwd.flat = 0x007f;
    guest_call<Conv::cdecl_, uint32_t>(s, kCodePrecision, value);
    float product = static_cast<float>(5474.0 * static_cast<double>(1.0f / 3.0f));
    CHECK(mem::read<double>(value + 8) == static_cast<double>(std::sqrt(product)));
    CHECK(mem::read<double>(value + 8) != exact);
    s.x87.fxsave32.cwd.flat = saved_cw;
    CHECK(esp(s) == before);

    for (unsigned i = 0; i < 4; ++i) {
        mem::write<float>(value + 4 * i, static_cast<float>(i + 1));
    }
    guest_call<Conv::cdecl_, uint32_t>(s, kCodeExchange, value);
    CHECK(mem::read<float>(value + 16) == 1.0f);
    CHECK(s.x87.fxsave32.swd.top == top);

    mem::write<float>(value, 1.0f);
    mem::write<float>(value + 4, -1.0f);
    guest_call<Conv::cdecl_, uint32_t>(s, kCodeArctan, value);
    CHECK(mem::read<double>(value + 8) == std::atan2(1.0, -1.0));
    mem::write<float>(value, -0.0f);
    mem::write<float>(value + 4, 0.0f);
    guest_call<Conv::cdecl_, uint32_t>(s, kCodeArctan, value);
    CHECK(mem::read<double>(value + 8) == 0.0 && std::signbit(mem::read<double>(value + 8)));
    CHECK(esp(s) == before);
}

void test_wsprintf() {
    auto args = [](std::vector<uint32_t> v) { return [v](unsigned i) { return i < v.size() ? v[i] : 0u; }; };
    auto strings = [](uint32_t addr) { return addr == 1 ? std::string("gta") : std::string("?"); };
    CHECK(win32::format_wsprintf("%d|%5d|%-5d|%05d", args({static_cast<uint32_t>(-42), 7, 7, static_cast<uint32_t>(-7)}),
                                 strings) == "-42|    7|7    |-0007");
    CHECK(win32::format_wsprintf("%x %X %#x %08lx", args({255, 255, 255, 0xbeef}), strings) == "ff FF 0xff 0000beef");
    CHECK(win32::format_wsprintf("%s_%c%%%.2s", args({1, 'Z', 1}), strings) == "gta_Z%gt");
    CHECK(win32::format_wsprintf("%u", args({0xffffffffu}), strings) == "4294967295");
}


void test_allocator_realloc() {
    GuestAllocator& a = allocator();
    uint32_t big = a.alloc(1 << 20);
    CHECK(big != 0 && a.size_of(big) == (1u << 20));
    mem::write<uint32_t>(big + 0x1000, 0xfeedface);
    uint32_t shrunk = a.realloc(big, 154524);
    CHECK(shrunk == big);
    CHECK(a.size_of(big) == 154524);
    CHECK(mem::read<uint32_t>(big + 0x1000) == 0xfeedface);
    uint32_t regrown = a.realloc(big, 200000);
    CHECK(regrown == big && a.size_of(big) == 200000);
    CHECK(mem::read<uint32_t>(big + 154524) == 0);
    uint32_t moved = a.realloc(big, 4 << 20);
    CHECK(moved != big && mem::read<uint32_t>(moved + 0x1000) == 0xfeedface);
    CHECK(!a.owns(big));
    uint32_t other = a.alloc(1 << 20);
    CHECK(other == big);
    a.free(other);
    a.free(moved);
    uint32_t small = a.alloc(24);
    uint32_t grown = a.realloc(small, 40);
    CHECK(grown != small && a.size_of(grown) == 40);
    a.free(grown);
}

void test_mode_list() {
    DisplayMode m;
    CHECK(parse_mode("1920x1080", m) && m == (DisplayMode{1920, 1080}));
    CHECK(!parse_mode("1920x1080x32", m));
    CHECK(!parse_mode("320x200", m));
    CHECK(!parse_mode("wide", m));
    auto list = build_mode_list({1600, 900}, {}, {1920, 1200});
    CHECK(list.front() == (DisplayMode{1600, 900}));
    CHECK(std::ranges::count(list, DisplayMode{1600, 900}) == 1);
    CHECK(std::ranges::find(list, DisplayMode{1920, 1200}) != list.end());
    CHECK(std::ranges::find(list, DisplayMode{2560, 1440}) == list.end());
    CHECK(list[1] == (DisplayMode{1280, 720}));
    auto chosen = build_mode_list({1280, 720}, {{1920, 1080}, {1280, 720}, {1024, 768}}, {1920, 1200});
    CHECK((chosen == std::vector<DisplayMode>{{1280, 720}, {1024, 768}, {1920, 1080}}));
    auto headless = build_mode_list({1280, 720}, {}, {});
    CHECK(headless.front() == (DisplayMode{1280, 720}) && headless.size() > 5);
    WindowMode mode = WindowMode::windowed;
    CHECK(parse_window_mode("borderless", mode) && mode == WindowMode::borderless);
    CHECK(parse_window_mode("fullscreen", mode) && mode == WindowMode::fullscreen);
    CHECK(!parse_window_mode("Fullscreen", mode) && mode == WindowMode::fullscreen);
    CHECK(window_mode_name(WindowMode::windowed) == "windowed");
}

void test_viewport() {
    using namespace platform;
    auto exact = fit_content(640, 360, 1280, 720);
    CHECK(exact.x == 0 && exact.y == 0 && exact.width == 640 && exact.height == 360);
    auto pillar = fit_content(1000, 360, 1280, 720);
    CHECK(pillar.x == 180 && pillar.y == 0 && pillar.width == 640 && pillar.height == 360);
    auto letter = fit_content(640, 500, 1280, 720);
    CHECK(letter.x == 0 && letter.y == 70 && letter.height == 360);
    auto top_left = view_to_client(pillar, 180, 360, 1280, 720);
    CHECK(top_left.x == 0 && top_left.y == 0);
    auto bottom_right = view_to_client(pillar, 820, 0, 1280, 720);
    CHECK(bottom_right.x == 1280 && bottom_right.y == 720);
    auto outside = view_to_client(pillar, 100, 180, 1280, 720);
    CHECK(outside.x < 0);
    auto back = client_to_view(letter, 640, 360, 1280, 720);
    CHECK(back.x == 320 && back.y == 250);
    auto round = view_to_client(letter, back.x, back.y, 1280, 720);
    CHECK(round.x == 640 && round.y == 360);
    auto clamped = clamp_client({-5, 900}, 1280, 720);
    CHECK(clamped.x == 0 && clamped.y == 719);
    auto empty = fit_content(0, 0, 1280, 720);
    CHECK(empty.width == 0 && view_to_client(empty, 1, 1, 1280, 720).x == 0);
}

void test_frame_stats() {
    std::vector<float> frames(100, 16.0f);
    frames[10] = 40.0f;
    frames[20] = 25.0f;
    auto s = perf::summarize(frames);
    CHECK(s.frames == 100 && s.p50_ms == 16.0f && s.max_ms == 40.0f);
    CHECK(s.over_20ms == 2 && s.over_33ms == 1);
    CHECK(s.p99_ms == 25.0f);
    CHECK(std::fabs(s.mean_ms - 16.33) < 0.01);
    CHECK(perf::summarize({}).frames == 0);
    CHECK(perf::format_stats(s).starts_with("100 frames, mean 16.33 ms"));
}

void test_key_table() {
    const input::Key* ret = input::key_for_mac(0x24);
    CHECK(ret && ret->vk == 0x0d && ret->scan == 0x1c && !ret->extended && input::dik(*ret) == 0x1c);
    const input::Key* up = input::key_for_name("UP");
    CHECK(up && up->mac == 0x7e && up->vk == 0x26 && input::dik(*up) == 0xc8);
    const input::Key* rctrl = input::key_for_name("rctrl");
    CHECK(rctrl && rctrl->vk == input::kVkControl && rctrl->side_vk == input::kVkRControl && input::dik(*rctrl) == 0x9d);
    CHECK(input::key_for_name("enter") == ret);
    CHECK(input::key_for_name("nosuchkey") == nullptr);
    CHECK(input::other_side(input::kVkLShift) == input::kVkRShift);
    CHECK(input::other_side(input::kVkRMenu) == input::kVkLMenu);
    for (uint16_t mac = 0; mac < 0x80; ++mac) {
        const input::Key* k = input::key_for_mac(mac);
        CHECK(!k || input::key_for_name(k->name) == k);
    }
}

void test_key_script() {
    std::string error;
    auto steps = input::parse_key_script("10:return,5m:space,20:lshift+,30:lshift-", error);
    CHECK(error.empty());
    CHECK(steps.size() == 4);
    CHECK(steps[0].movie && steps[0].frame == 5 && steps[0].down && steps[0].tap);
    CHECK(!steps[1].movie && steps[1].frame == 10 && steps[1].key == input::key_for_name("return") && steps[1].tap);
    CHECK(steps[2].frame == 20 && steps[2].down && !steps[2].tap);
    CHECK(steps[3].frame == 30 && !steps[3].down && !steps[3].tap);
    CHECK(input::parse_key_script("x:return", error).empty() && !error.empty());
    CHECK(input::parse_key_script("10:bogus", error).empty() && error.find("bogus") != std::string::npos);
    CHECK(input::parse_key_script("10return", error).empty());
    auto save = input::parse_key_script("40:save3", error);
    CHECK(save.size() == 1 && save[0].kind == input::StepKind::save && save[0].button == 2 && !save[0].tap);
    CHECK(input::parse_key_script("40:save9", error).empty());
    CHECK(input::parse_key_script("40:save3+", error).empty());
    unsigned saved_slot = 99;
    input::set_script_sink({.save = [&](unsigned slot) { saved_slot = slot; }});
    input::set_key_script(save);
    input::run_key_script(false, 39);
    CHECK(saved_slot == 99);
    input::run_key_script(false, 40);
    CHECK(saved_slot == 2);
    input::set_key_script({});

    std::vector<std::pair<std::string, bool>> fired;
    input::set_script_sink({.key = [&](const input::Key& k, bool down) { fired.emplace_back(std::string(k.name), down); }});
    input::set_key_script(steps);
    input::run_key_script(false, 9);
    CHECK(fired.empty());
    input::run_key_script(true, 6);
    CHECK(fired.size() == 1 && fired[0].first == "space" && fired[0].second);
    input::poll_key_script();
    CHECK(fired.size() == 1);
    input::note_idle_peek();
    input::poll_key_script();
    CHECK(fired.size() == 1);
    input::run_key_script(true, 7);
    CHECK(fired.size() == 1);
    input::poll_key_script();
    CHECK(fired.size() == 2 && fired[1].first == "space" && !fired[1].second);
    input::run_key_script(false, 25);
    CHECK(fired.size() == 4);
    CHECK(fired[2].first == "return" && fired[2].second);
    CHECK(fired[3].first == "lshift" && fired[3].second);
    input::poll_key_script();
    CHECK(fired.size() == 4);
    input::note_idle_peek();
    input::poll_key_script();
    CHECK(fired.size() == 4);
    input::run_key_script(false, 26);
    input::poll_key_script();
    CHECK(fired.size() == 5 && fired[4].first == "return" && !fired[4].second);
    input::run_key_script(false, 100);
    CHECK(fired.size() == 6 && fired[5].first == "lshift" && !fired[5].second);
    input::note_idle_peek();
    input::poll_key_script();
    input::run_key_script(false, 200);
    CHECK(fired.size() == 6);
    input::set_key_script({});
    input::set_script_sink({});

    auto mouse = input::parse_key_script("5:lbutton@100:200,6:move@-3:4,7:wheeldown,8:rbutton+,9:rbutton-", error);
    CHECK(mouse.size() == 5);
    CHECK(mouse[0].kind == input::StepKind::button && mouse[0].button == 0 && mouse[0].has_pos && mouse[0].x == 100 &&
          mouse[0].y == 200 && mouse[0].tap);
    CHECK(mouse[1].kind == input::StepKind::move && mouse[1].x == -3 && mouse[1].y == 4 && !mouse[1].tap);
    CHECK(mouse[2].kind == input::StepKind::wheel && mouse[2].wheel == -120);
    CHECK(mouse[3].button == 1 && mouse[3].down && !mouse[3].tap);
    CHECK(input::parse_key_script("5:move", error).empty());
    CHECK(input::parse_key_script("5:lbutton@1", error).empty());
    CHECK(input::parse_key_script("5:wheelup+", error).empty());

    std::vector<std::string> events;
    input::set_script_sink({
        [&](const input::Key& k, bool down) { events.push_back(std::string(k.name) + (down ? "+" : "-")); },
        [&](int32_t x, int32_t y) { events.push_back("move" + std::to_string(x) + ":" + std::to_string(y)); },
        [&](unsigned b, bool down) { events.push_back("button" + std::to_string(b) + (down ? "+" : "-")); },
        [&](int32_t d) { events.push_back("wheel" + std::to_string(d)); },
        [&](bool active) { events.push_back(active ? "focus" : "blur"); },
    });
    input::set_key_script(mouse);
    input::run_key_script(false, 7);
    CHECK((events == std::vector<std::string>{"move100:200", "button0+", "move-3:4", "wheel-120"}));
    input::note_idle_peek();
    input::poll_key_script();
    CHECK(events.size() == 4);
    input::run_key_script(false, 9);
    CHECK(events.size() == 6 && events[4] == "button1+" && events[5] == "button1-");
    input::poll_key_script();
    CHECK(events.size() == 7 && events[6] == "button0-");
    auto timed = input::parse_key_script("0s:blur,3:focus,1s:return", error);
    CHECK(timed.size() == 3);
    CHECK(timed[0].timed && timed[0].frame == 0 && timed[0].kind == input::StepKind::focus && !timed[0].down);
    CHECK(!timed[1].timed && timed[1].frame == 3 && timed[1].down);
    CHECK(timed[2].timed && timed[2].frame == 1000 && timed[2].tap);
    CHECK(input::parse_key_script("s:blur", error).empty());
    CHECK(input::parse_key_script("1:blur+", error).empty());
    events.clear();
    input::set_key_script(timed);
    input::poll_key_script();
    CHECK((events == std::vector<std::string>{"blur"}));
    input::run_key_script(false, 3);
    CHECK(events.size() == 2 && events[1] == "focus");
    input::set_key_script({});
    input::set_script_sink({});
}

template <GuestScalar... Args>
uint32_t vcall(GuestState& s, uint32_t obj, unsigned index, Args... args) {
    uint32_t target = mem::read<uint32_t>(mem::read<uint32_t>(obj) + 4 * index);
    return guest_call<Conv::stdcall, uint32_t>(s, target, obj, args...);
}

void put_guid(uint32_t at, uint32_t d1, uint16_t d2, uint16_t d3, std::array<uint8_t, 8> d4) {
    mem::write<uint32_t>(at, d1);
    mem::write<uint16_t>(at + 4, d2);
    mem::write<uint16_t>(at + 6, d3);
    std::memcpy(mem::ptr(at + 8), d4.data(), 8);
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

void test_audio_mix() {
    CHECK(audio::db_gain(0) == 1);
    CHECK(audio::db_gain(-10000) == 0);
    CHECK(near(audio::db_gain(-2000), 0.1f));
    CHECK(near(audio::pan_gains(10000).left, 0) && audio::pan_gains(10000).right == 1);
    CHECK(audio::pan_gains(-600).left == 1 && near(audio::pan_gains(-600).right, audio::db_gain(-600)));

    audio::Listener l;
    l.front = {0, 1, 0};
    l.top = {0, 0, -1};
    l.rolloff = 0;
    audio::Spatial src;
    src.position = {10, 0, 0};
    auto g = audio::spatial_gains(l, src);
    CHECK(near(g.left, 0) && near(g.right, 1));
    src.position = {-10, 0, 0};
    g = audio::spatial_gains(l, src);
    CHECK(near(g.left, 1) && near(g.right, 0));
    src.position = {0, 10, 0};
    g = audio::spatial_gains(l, src);
    CHECK(near(g.left, 1) && near(g.right, 1));
    src.position = {5, 5, 0};
    g = audio::spatial_gains(l, src);
    CHECK(g.right == 1 && g.left > 0.4f && g.left < 0.7f);
    l.rolloff = 1;
    src.position = {0, 4, 0};
    g = audio::spatial_gains(l, src);
    CHECK(near(g.left, 0.25f) && near(g.right, 0.25f));
    src.mode = audio::kMode3dDisable;
    g = audio::spatial_gains(l, src);
    CHECK(g.left == 1 && g.right == 1);

    int16_t pcm[4] = {0, 16384, 0, -16384};
    audio::Voice v;
    v.data = reinterpret_cast<const uint8_t*>(pcm);
    v.bytes = sizeof(pcm);
    v.format = {1, 16, 2, 24000};
    std::array<float, 32> out{};
    double pos = 0;
    CHECK(!audio::mix_voice(out.data(), 16, 48000, v, pos));
    CHECK(near(out[0], 0) && near(out[2], 0.25f) && near(out[4], 0.5f) && near(out[5], 0.5f) && near(out[6], 0.25f));
    CHECK(near(out[12], -0.5f) && out[16] == 0);
    out.fill(0);
    pos = 0;
    v.looping = true;
    CHECK(audio::mix_voice(out.data(), 16, 48000, v, pos));
    CHECK(near(out[16], 0) && near(out[20], 0.5f) && near(pos, 0));
    out.fill(0);
    pos = 0;
    v.frequency = 48000;
    v.gains = {0.5f, 0};
    CHECK(audio::mix_voice(out.data(), 2, 48000, v, pos));
    CHECK(near(out[2], 0.25f) && out[3] == 0 && near(pos, 2));
}

void test_audio_stream() {
    auto stream = std::make_shared<audio::Stream>(24000);
    float in[8] = {0, 0, 0.5f, -0.5f, 1, -1, 0.5f, -0.5f};
    stream->push(in, 4);
    CHECK(stream->queued() == 4);
    std::array<float, 16> out{};
    audio::add_stream(stream);
    audio::mix_streams(out.data(), 8, 48000);
    CHECK(near(out[0], 0) && near(out[2], 0.25f) && near(out[3], -0.25f) && near(out[4], 0.5f));
    CHECK(near(out[8], 1) && near(out[10], 0.75f) && out[12] == 0 && out[14] == 0);
    CHECK(stream->queued() == 1);
    stream->push(in, 4);
    out.fill(0);
    audio::mix_streams(out.data(), 2, 48000);
    CHECK(near(out[0], 0.5f) && near(out[2], 0.25f));
    audio::remove_stream(stream.get());
    out.fill(0);
    audio::mix_streams(out.data(), 8, 48000);
    CHECK(out[0] == 0);
    stream->clear();
    CHECK(stream->queued() == 0);
}

void test_dsound(GuestState& s) {
    settings().audio_clockless = true;
    uint32_t mem_base = allocator().reserve(0x10000);
    uint32_t out = mem_base;
    uint32_t desc = mem_base + 0x100;
    uint32_t wfx = mem_base + 0x200;
    uint32_t iid = mem_base + 0x300;
    uint32_t p1 = mem_base + 0x400;
    uint32_t b1 = mem_base + 0x404;
    uint32_t play = mem_base + 0x410;
    uint32_t write = mem_base + 0x414;
    uint32_t created = guest_call<Conv::stdcall, uint32_t>(s, thunk_for("dsound.dll", "DirectSoundCreate8", 0), 0u, out, 0u);
    CHECK(created == 0);
    uint32_t ds = mem::read<uint32_t>(out);

    std::memset(mem::ptr(desc), 0, 36);
    mem::write<uint32_t>(desc, 36);
    mem::write<uint32_t>(desc + 4, 0x1 | 0x10);
    CHECK(vcall(s, ds, 3, desc, out, 0u) == 0);
    uint32_t primary = mem::read<uint32_t>(out);
    put_guid(iid, 0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60});
    CHECK(vcall(s, primary, 0, iid, out) == 0);
    uint32_t listener = mem::read<uint32_t>(out);
    CHECK(vcall(s, listener, 13, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1u) == 0);
    CHECK(vcall(s, listener, 15, 0.0f, 1u) == 0);
    CHECK(vcall(s, listener, 17) == 0);

    mem::write<uint16_t>(wfx, 1);
    mem::write<uint16_t>(wfx + 2, 1);
    mem::write<uint32_t>(wfx + 4, 24000);
    mem::write<uint32_t>(wfx + 8, 48000);
    mem::write<uint16_t>(wfx + 12, 2);
    mem::write<uint16_t>(wfx + 14, 16);
    mem::write<uint32_t>(desc + 4, 0x10 | 0x80 | 0x20);
    mem::write<uint32_t>(desc + 8, 2400);
    mem::write<uint32_t>(desc + 16, wfx);
    CHECK(vcall(s, ds, 3, desc, out, 0u) == 0);
    uint32_t buffer = mem::read<uint32_t>(out);
    CHECK(vcall(s, buffer, 11, 0u, 2400u, p1, b1, 0u, 0u, 0u) == 0);
    CHECK(mem::read<uint32_t>(b1) == 2400);
    for (uint32_t i = 0; i < 1200; ++i) {
        mem::write<int16_t>(mem::read<uint32_t>(p1) + 2 * i, 16384);
    }
    CHECK(vcall(s, buffer, 19, mem::read<uint32_t>(p1), 2400u, 0u, 0u) == 0);
    put_guid(iid, 0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60});
    CHECK(vcall(s, buffer, 0, iid, out) == 0);
    uint32_t b3d = mem::read<uint32_t>(out);
    CHECK(vcall(s, b3d, 19, 10.0f, 0.0f, 0.0f, 1u) == 0);

    std::vector<float> mix(2 * 480);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(mix[0] == 0 && mix[1] == 0);
    CHECK(vcall(s, buffer, 12, 0u, 0u, 0u) == 0);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(near(mix[0], 0.5f) && near(mix[1], 0.5f));
    CHECK(vcall(s, listener, 17) == 0);
    std::fill(mix.begin(), mix.end(), 0.0f);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(near(mix[0], 0) && near(mix[1], 0.5f));
    CHECK(vcall(s, buffer, 4, play, write) == 0);
    CHECK(mem::read<uint32_t>(play) == 960);
    CHECK(mem::read<uint32_t>(write) == 960 + 480);
    CHECK(vcall(s, buffer, 15, static_cast<uint32_t>(-2000)) == 0);
    std::fill(mix.begin(), mix.end(), 0.0f);
    win32::dsound_render(mix.data(), 100, 48000);
    CHECK(near(mix[1], 0.05f));
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(vcall(s, buffer, 9, out) == 0);
    CHECK(mem::read<uint32_t>(out) == 1);
    win32::dsound_render(mix.data(), 480, 48000);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(vcall(s, buffer, 9, out) == 0);
    CHECK(mem::read<uint32_t>(out) == 0);
    CHECK(vcall(s, buffer, 4, play, write) == 0);
    CHECK(mem::read<uint32_t>(play) == 0);

    CHECK(vcall(s, buffer, 12, 0u, 0u, 1u) == 0);
    CHECK(vcall(s, buffer, 9, out) == 0);
    CHECK(mem::read<uint32_t>(out) == 0x5);
    win32::dsound_render(mix.data(), 480, 48000);
    win32::dsound_render(mix.data(), 480, 48000);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(vcall(s, buffer, 9, out) == 0);
    CHECK(mem::read<uint32_t>(out) == 0x5);
    CHECK(vcall(s, buffer, 4, play, write) == 0);
    CHECK(mem::read<uint32_t>(play) == 1440);
    CHECK(vcall(s, buffer, 18) == 0);
    std::fill(mix.begin(), mix.end(), 0.0f);
    win32::dsound_render(mix.data(), 480, 48000);
    CHECK(mix[1] == 0);
    CHECK(vcall(s, buffer, 4, play, write) == 0);
    CHECK(mem::read<uint32_t>(play) == 1440);
    CHECK(vcall(s, b3d, 2) != 0);
    CHECK(vcall(s, buffer, 2) == 0);
}

void test_joystick(GuestState& s) {
    uint32_t base = allocator().reserve(0x10000);
    uint32_t out = base;
    uint32_t guid = base + 0x100;
    uint32_t fmt = base + 0x200;
    uint32_t prop = base + 0x300;
    uint32_t info = base + 0x400;
    uint32_t state = base + 0x1000;
    uint32_t created =
        guest_call<Conv::stdcall, uint32_t>(s, thunk_for("dinput8.dll", "DirectInput8Create", 0), 0u, 0x800u, 0u, out, 0u);
    CHECK(created == 0);
    uint32_t di = mem::read<uint32_t>(out);
    g_enumerated.clear();
    CHECK(vcall(s, di, 4, 4u, api("enum_cb"), 0u, 1u) == 0);
    CHECK(g_enumerated.size() == 1 && g_enumerated[0].devtype == 0x10215);
    g_enumerated.clear();
    CHECK(vcall(s, di, 4, 0u, api("enum_cb"), 0u, 1u) == 0);
    CHECK(g_enumerated.size() == 3);
    g_enumerated.clear();
    CHECK(vcall(s, di, 4, 4u, api("enum_cb"), 0u, 1u) == 0);
    std::memcpy(mem::ptr(guid), g_enumerated[0].guid.data(), 16);
    CHECK(vcall(s, di, 3, guid, out, 0u) == 0);
    uint32_t joy = mem::read<uint32_t>(out);
    std::memset(mem::ptr(fmt), 0, 24);
    mem::write<uint32_t>(fmt + 12, 272);
    CHECK(vcall(s, joy, 11, fmt) == 0);
    mem::write<uint32_t>(prop, 24);
    mem::write<uint32_t>(prop + 4, 16);
    mem::write<uint32_t>(prop + 12, 1);
    mem::write<uint32_t>(prop + 16, static_cast<uint32_t>(-2000));
    mem::write<uint32_t>(prop + 20, 2000);
    mem::write<uint32_t>(info, 0x13c);
    for (uint32_t offset : {0u, 4u, 8u, 20u}) {
        CHECK(vcall(s, joy, 14, info, offset, 1u) == 0);
        CHECK(mem::read<uint32_t>(info + 20) == offset);
        mem::write<uint32_t>(prop + 8, offset);
        CHECK(vcall(s, joy, 6, 4u, prop) == 0);
    }
    CHECK(vcall(s, joy, 14, info, 12u, 1u) != 0);
    mem::write<uint32_t>(prop + 8, 12);
    CHECK(vcall(s, joy, 6, 4u, prop) != 0);
    CHECK(vcall(s, joy, 14, info, 48u, 1u) == 0);
    mem::write<uint32_t>(prop, 20);
    mem::write<uint32_t>(prop + 8, 0);
    mem::write<uint32_t>(prop + 12, 0);
    CHECK(vcall(s, joy, 5, 24u, prop) == 0);
    CHECK((mem::read<uint32_t>(prop + 16) & 0xffff) != 0x3427);
    mem::write<uint32_t>(base + 0x500, 0x2c);
    CHECK(vcall(s, joy, 3, base + 0x500) == 0);
    CHECK(mem::read<uint32_t>(base + 0x500 + 12) == 4 && mem::read<uint32_t>(base + 0x500 + 16) == 16);

    CHECK(vcall(s, joy, 25) == 0x8007000cu);
    CHECK(vcall(s, joy, 9, 272u, state) != 0);
    CHECK(vcall(s, joy, 7) == 0);
    CHECK(vcall(s, joy, 25) == 0);

    input::set_script_pad_stick(0, 1.0f, 1.0f);
    input::set_script_pad_stick(1, -0.5f, 0.0f);
    input::set_script_pad_button(input::PadButton::south, true);
    input::set_script_pad_button(input::PadButton::start, true);
    input::set_script_pad_button(input::PadButton::left, true);
    CHECK(vcall(s, joy, 9, 272u, state) == 0);
    CHECK(static_cast<int32_t>(mem::read<uint32_t>(state)) == 2000);
    CHECK(static_cast<int32_t>(mem::read<uint32_t>(state + 4)) == -2000);
    CHECK(static_cast<int32_t>(mem::read<uint32_t>(state + 8)) == -1000);
    CHECK(static_cast<int32_t>(mem::read<uint32_t>(state + 20)) == 0);
    CHECK(mem::read<uint32_t>(state + 32) == 0xffffffffu);
    CHECK(mem::read<uint8_t>(state + 48) == 0 && mem::read<uint8_t>(state + 49) == 0x80);
    CHECK(mem::read<uint8_t>(state + 48 + 11) == 0x80 && mem::read<uint8_t>(state + 48 + 15) == 0x80);
    input::reset();
    CHECK(vcall(s, joy, 9, 272u, state) == 0);
    CHECK(mem::read<uint32_t>(state) == 0 && mem::read<uint32_t>(state + 4) == 0);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(mem::read<uint8_t>(state + 48 + i) == 0);
    }
    CHECK(vcall(s, joy, 2) == 0);

    std::string error;
    auto steps = input::parse_key_script("5:pad.a,6:lstick@50:-100,7:pad.Start+,8:rstick@0:0", error);
    CHECK(steps.size() == 4);
    CHECK(steps[0].kind == input::StepKind::pad_button && steps[0].tap &&
          steps[0].pad_button == static_cast<uint8_t>(input::PadButton::south));
    CHECK(steps[1].kind == input::StepKind::pad_stick && steps[1].button == 0 && steps[1].x == 50 && steps[1].y == -100);
    CHECK(steps[2].pad_button == static_cast<uint8_t>(input::PadButton::start) && steps[2].down && !steps[2].tap);
    CHECK(steps[3].button == 1);
    CHECK(input::parse_key_script("5:lstick", error).empty());
    CHECK(input::parse_key_script("5:pad.bogus", error).empty());
    CHECK(input::parse_key_script("5:lstick@200:0", error).empty());
    CHECK(input::parse_key_script("5:pad.a@1:2", error).empty());
    input::set_script_sink({});
    input::set_key_script(steps);
    input::run_key_script(false, 6);
    CHECK(input::pad_state().pressed(input::PadButton::south));
    CHECK(near(input::pad_state().lx, 0.5f) && near(input::pad_state().ly, -1.0f));
    input::note_idle_peek();
    input::run_key_script(false, 6);
    input::poll_key_script();
    CHECK(!input::pad_state().pressed(input::PadButton::south));
    input::set_key_script({});
    input::reset();
}

void test_keyboard_messages(GuestState& s) {
    input::reset();
    uint32_t msg = allocator().reserve(0x1000);
    uint32_t peek = thunk_for("USER32.dll", "PeekMessageA", 0);
    uint32_t get_key_state = thunk_for("USER32.dll", "GetKeyState", 0);
    auto next = [&] {
        uint32_t got = guest_call<Conv::stdcall, uint32_t>(s, peek, msg, 0u, 0x100u, 0x108u, 1u);
        return got ? std::array<uint32_t, 3>{mem::read<uint32_t>(msg + 4), mem::read<uint32_t>(msg + 8),
                                             mem::read<uint32_t>(msg + 12)}
                   : std::array<uint32_t, 3>{0, 0, 0};
    };

    const input::Key& up = *input::key_for_name("up");
    win32::key_event(0x1234, up, true, false);
    win32::key_event(0x1234, up, true, true);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, 0x26u) & 0x8000) != 0);
    win32::key_event(0x1234, up, false, false);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, 0x26u) & 0x8000) == 0);
    CHECK((next() == std::array<uint32_t, 3>{0x100, 0x26, 0x01480001}));
    CHECK((next() == std::array<uint32_t, 3>{0x100, 0x26, 0x41480001}));
    CHECK((next() == std::array<uint32_t, 3>{0x101, 0x26, 0xc1480001}));

    const input::Key& rshift = *input::key_for_name("rshift");
    win32::key_event(0x1234, rshift, true, false);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkRShift}) & 0x8000) != 0);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkShift}) & 0x8000) != 0);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkLShift}) & 0x8000) == 0);
    CHECK((next() == std::array<uint32_t, 3>{0x100, 0x10, 0x00360001}));
    win32::key_event(0x1234, rshift, false, false);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkShift}) & 0x8000) == 0);
    CHECK((next() == std::array<uint32_t, 3>{0x101, 0x10, 0xc0360001}));

    const input::Key& lalt = *input::key_for_name("lalt");
    const input::Key& x = *input::key_for_name("x");
    win32::key_event(0x1234, lalt, true, false);
    win32::key_event(0x1234, x, true, false);
    win32::key_event(0x1234, x, false, false);
    win32::key_event(0x1234, lalt, false, false);
    CHECK((next() == std::array<uint32_t, 3>{0x104, 0x12, 0x20380001}));
    CHECK((next() == std::array<uint32_t, 3>{0x104, 'X', 0x202d0001}));
    CHECK((next() == std::array<uint32_t, 3>{0x105, 'X', 0xe02d0001}));
    CHECK((next() == std::array<uint32_t, 3>{0x105, 0x12, 0xe0380001}));
    CHECK((next() == std::array<uint32_t, 3>{0, 0, 0}));

    const input::Key& caps = *input::key_for_name("capslock");
    uint32_t before = guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkCapital}) & 1;
    win32::key_event(0x1234, caps, true, false);
    win32::key_event(0x1234, caps, false, false);
    CHECK((guest_call<Conv::stdcall, uint32_t>(s, get_key_state, uint32_t{input::kVkCapital}) & 1) != before);
    next();
    next();

    uint8_t dik[256] = {};
    win32::key_event(0x1234, up, true, false);
    input::write_keyboard_state(dik, sizeof(dik));
    CHECK(dik[0xc8] == 0x80);
    win32::key_event(0x1234, up, false, false);
    input::write_keyboard_state(dik, sizeof(dik));
    CHECK(dik[0xc8] == 0);
    next();
    next();

    auto next_mouse = [&] {
        uint32_t got = guest_call<Conv::stdcall, uint32_t>(s, peek, msg, 0u, 0x200u, 0x20au, 1u);
        return got ? std::array<uint32_t, 3>{mem::read<uint32_t>(msg + 4), mem::read<uint32_t>(msg + 8),
                                             mem::read<uint32_t>(msg + 12)}
                   : std::array<uint32_t, 3>{0, 0, 0};
    };
    win32::mouse_move(0x1234, 10, 20);
    win32::mouse_move(0x1234, 30, 40);
    win32::mouse_button(0x1234, 0, true);
    win32::mouse_move(0x1234, -5, 50);
    win32::mouse_button(0x1234, 0, false);
    win32::mouse_wheel(0x1234, -120);
    CHECK((next_mouse() == std::array<uint32_t, 3>{0x200, 0, (40u << 16) | 30u}));
    CHECK((next_mouse() == std::array<uint32_t, 3>{0x201, 1, (40u << 16) | 30u}));
    CHECK((next_mouse() == std::array<uint32_t, 3>{0x200, 1, (50u << 16) | 0xfffbu}));
    CHECK((next_mouse() == std::array<uint32_t, 3>{0x202, 0, (50u << 16) | 0xfffbu}));
    CHECK((next_mouse() == std::array<uint32_t, 3>{0x20a, 0xff880000u, (50u << 16) | 0xfffbu}));
    CHECK((next_mouse() == std::array<uint32_t, 3>{0, 0, 0}));
    uint8_t dims[16] = {};
    input::write_mouse_state(dims, sizeof(dims));
    CHECK(static_cast<int32_t>(dims[8] | dims[9] << 8 | dims[10] << 16 | dims[11] << 24) == -120);
    CHECK(dims[12] == 0);

    auto axis = [&](size_t at) {
        return static_cast<int32_t>(dims[at] | dims[at + 1] << 8 | dims[at + 2] << 16 | dims[at + 3] << 24);
    };
    input::add_mouse_motion(0.4, -0.75);
    input::write_mouse_state(dims, sizeof(dims));
    CHECK(axis(0) == 0);
    CHECK(axis(4) == 0);
    input::add_mouse_motion(0.4, -0.75);
    input::add_mouse_motion(0.4, 0.0);
    input::write_mouse_state(dims, sizeof(dims));
    CHECK(axis(0) == 1);
    CHECK(axis(4) == -1);
    input::add_mouse_motion(-0.2, 0.5);
    input::write_mouse_state(dims, sizeof(dims));
    CHECK(axis(0) == 0);
    CHECK(axis(4) == 0);
    input::reset();
}

}

int main() {
    if (!reserve_guest_space()) {
        std::fprintf(stderr, "cannot reserve guest space\n");
        return 1;
    }
    win32::register_all();
    register_apis(kTestApis);
    register_apis(kReenterApi);
    register_trace(kGuestAddTwice, guest_add_twice);
    register_trace(kGuestReenter, guest_reenter);
    register_trace(kGuestE, guest_e);
    register_trace(kGuestF, guest_f);
    register_trace(kGuestG, guest_g);
    register_trace(kHandlerSearch, handler_search);
    register_trace(kHandlerContinue, handler_continue);
    register_trace(kHandlerExcept, handler_except);

    uint32_t stack = allocator().reserve(0x40000);
    GuestThread* thread = create_guest_thread(stack, stack + 0x40000);
    bind_current_thread(thread);
    GuestState& s = thread->state;

    test_calling_conventions(s);
    test_native_guest_bridge(s);
    test_seh_chain(s);
    test_seh_nonlocal_unwind(s);
    test_interpreter(s);
    test_wsprintf();
    test_allocator_realloc();
    test_mode_list();
    test_viewport();
    test_frame_stats();
    test_key_table();
    test_key_script();
    test_keyboard_messages(s);
    test_audio_mix();
    test_audio_stream();
    test_dsound(s);
    test_joystick(s);
    return ma::test::finish("host_runtime");
}
