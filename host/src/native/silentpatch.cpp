#include "natives.hpp"

#include "../mod_settings.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <string_view>

namespace ma::host::native {

namespace {

constexpr uint32_t kGetLookBehindForCar = 0x0053fe70;
constexpr uint32_t kJetpackHover = 0x0067edaf;

uint32_t guest_literal(std::string_view text) {
    uint32_t addr = allocator().alloc(static_cast<uint32_t>(text.size() + 1));
    mem::write_cstr(addr, text);
    return addr;
}

Memory* ascii_to_gxt_null_check(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t fallback = guest_literal("-");
    static std::atomic<bool> reached = false;
    static std::atomic<bool> substituted = false;
    uint32_t src = mem::read<uint32_t>(esp(s) + 4);
    uint32_t dest = mem::read<uint32_t>(esp(s) + 8);
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] resolution string: redirect reached at 0x%08x (\"%s\")\n", redirect_site(),
                     src ? mem::read_cstr(src, 64).c_str() : "");
    }
    if (!src) {
        src = fallback;
        if (!substituted.exchange(true)) {
            std::fprintf(stderr, "[SP] resolution string: missing, showing \"-\"\n");
        }
    }
    guest_call<Conv::cdecl_>(s, redirect_original(), src, dest);
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
    return m;
}

bool locale_uses_metric() {
    CFLocaleRef locale = CFLocaleCopyCurrent();
    if (!locale) {
        return true;
    }
    bool metric = true;
    auto value = static_cast<CFBooleanRef>(CFLocaleGetValue(locale, kCFLocaleUsesMetricSystem));
    if (value && CFGetTypeID(value) == CFBooleanGetTypeID()) {
        metric = CFBooleanGetValue(value);
    }
    CFRelease(locale);
    return metric;
}

uint32_t is_metric() {
    static std::atomic<bool> logged = false;
    auto units = mod_settings().choice("silentpatch.units");
    bool metric = units == "metric" ? true : units == "imperial" ? false : locale_uses_metric();
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[SP] units: %s (silentpatch.units = %s)\n", metric ? "metric" : "imperial", units.c_str());
    }
    return metric ? 1 : 0;
}

uint32_t jetpack_keyboard_hover(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] jetpack: keyboard controls reach the hover hook\n");
    }
    uint32_t saved_eax = eax(s);
    uint32_t saved_ecx = ecx(s);
    uint32_t saved_edx = edx(s);
    auto pressed = guest_call<Conv::thiscall, uint8_t>(s, kGetLookBehindForCar, ebp(s));
    eax(s) = saved_eax;
    ecx(s) = saved_ecx;
    edx(s) = saved_edx;
    static std::atomic<bool> logged = false;
    if (pressed && !logged.exchange(true)) {
        std::fprintf(stderr, "[SP] jetpack: look-behind hovers (task 0x%08x)\n", esi(s));
    }
    return pressed ? kJetpackHover : pc;
}

constexpr NativeSymbol kSymbols[] = {
    {"sp_is_metric", native_trace<Conv::cdecl_, is_metric>},
    {"sp_ascii_to_gxt_null_check", ascii_to_gxt_null_check},
};

constexpr MidHookSymbol kMidHooks[] = {
    {"sp_jetpack_keyboard_hover", jetpack_keyboard_hover},
};

}

std::span<const NativeSymbol> silentpatch_symbols() { return kSymbols; }

std::span<const MidHookSymbol> silentpatch_mid_hooks() { return kMidHooks; }

}
