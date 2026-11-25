#include "check.hpp"

#include "abi.hpp"
#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "input/input_state.hpp"
#include "input/keys.hpp"
#include "thread.hpp"
#include "thunks.hpp"
#include "win32/process.hpp"
#include "win32/seh.hpp"
#include "win32/user32.hpp"
#include "win32/wsprintf.hpp"

#include "macandreas/interp.hpp"

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <cstdio>
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

constexpr ApiDef kTestApis[] = {
    native_export<Conv::stdcall, add3>("TEST.dll", "add3"),
    native_export<Conv::cdecl_, mul64>("TEST.dll", "mul64"),
    native_export<Conv::thiscall, member_add>("TEST.dll", "member_add"),
    native_export<Conv::stdcall, scale>("TEST.dll", "scale"),
    native_export<Conv::cdecl_, average>("TEST.dll", "average"),
    native_export<Conv::stdcall, note_call>("TEST.dll", "note_call"),
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

    std::vector<std::pair<std::string, bool>> fired;
    input::set_key_sink([&](const input::Key& k, bool down) { fired.emplace_back(std::string(k.name), down); });
    input::set_key_script(steps);
    input::run_key_script(false, 9);
    CHECK(fired.empty());
    input::run_key_script(true, 6);
    CHECK(fired.size() == 1 && fired[0].first == "space" && fired[0].second);
    input::poll_key_script();
    CHECK(fired.size() == 1);
    input::note_idle_peek();
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
    CHECK(fired.size() == 5 && fired[4].first == "return" && !fired[4].second);
    input::run_key_script(false, 100);
    CHECK(fired.size() == 6 && fired[5].first == "lshift" && !fired[5].second);
    input::note_idle_peek();
    input::poll_key_script();
    input::run_key_script(false, 200);
    CHECK(fired.size() == 6);
    input::set_key_script({});
    input::set_key_sink(nullptr);
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
    test_key_table();
    test_key_script();
    test_keyboard_messages(s);
    return ma::test::finish("host_runtime");
}
