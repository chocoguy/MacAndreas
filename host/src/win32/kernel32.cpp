#include "process.hpp"
#include "text.hpp"

#include "../audio/output.hpp"
#include "../perf.hpp"
#include "../crash.hpp"
#include "../thread.hpp"

#include <mach/mach_time.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kErrorModNotFound = 126;
constexpr uint32_t kErrorProcNotFound = 127;
constexpr uint32_t kErrorInvalidFlags = 1004;

struct HeapObject : KObject {};
uint32_t g_std_handles[3] = {};

std::mutex g_cs_mutex;
std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_critical_sections;

std::recursive_mutex& critical_section(uint32_t addr) {
    std::lock_guard lock(g_cs_mutex);
    auto& slot = g_critical_sections[addr];
    if (!slot) {
        slot = std::make_unique<std::recursive_mutex>();
    }
    return *slot;
}

uint64_t now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return static_cast<uint64_t>(static_cast<unsigned __int128>(mach_absolute_time()) * tb.numer / tb.denom);
}

uint32_t tick_ms() {
    static uint64_t start = now_ns();
    return static_cast<uint32_t>((now_ns() - start) / 1000000 + 0x10000);
}

uint32_t copy_out_a(uint32_t buffer, uint32_t size, std::string_view text) {
    if (size == 0) {
        return static_cast<uint32_t>(text.size() + 1);
    }
    uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(text.size()), size - 1);
    std::memcpy(mem::ptr(buffer), text.data(), n);
    mem::write<uint8_t>(buffer + n, 0);
    return n;
}

void write_systemtime(uint32_t p, const std::tm& t, int ms) {
    mem::write<uint16_t>(p + 0, static_cast<uint16_t>(t.tm_year + 1900));
    mem::write<uint16_t>(p + 2, static_cast<uint16_t>(t.tm_mon + 1));
    mem::write<uint16_t>(p + 4, static_cast<uint16_t>(t.tm_wday));
    mem::write<uint16_t>(p + 6, static_cast<uint16_t>(t.tm_mday));
    mem::write<uint16_t>(p + 8, static_cast<uint16_t>(t.tm_hour));
    mem::write<uint16_t>(p + 10, static_cast<uint16_t>(t.tm_min));
    mem::write<uint16_t>(p + 12, static_cast<uint16_t>(t.tm_sec));
    mem::write<uint16_t>(p + 14, static_cast<uint16_t>(ms));
}

constexpr ApiDef kApis[] = {
    {"KERNEL32.dll", "GetVersionExA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t p = c.arg(0);
         mem::write<uint32_t>(p + 4, 5);
         mem::write<uint32_t>(p + 8, 1);
         mem::write<uint32_t>(p + 12, 2600);
         mem::write<uint32_t>(p + 16, 2);
         mem::write_cstr(p + 20, "Service Pack 3");
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetCommandLineA", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(process().command_line_a); }},
    {"KERNEL32.dll", "GetStartupInfoA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t p = c.arg(0);
         std::memset(mem::ptr(p), 0, 68);
         mem::write<uint32_t>(p, 68);
     }},
    {"KERNEL32.dll", "GetModuleHandleA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t name = c.arg(0);
         if (name == 0) {
             c.ret(process().image_base);
             return;
         }
         auto n = mem::read_cstr(name);
         uint32_t h = module_handle_for(n);
         std::fprintf(stderr, "[MODULE] GetModuleHandleA(\"%s\")%s\n", n.c_str(), h ? "" : " -> not available");
         if (h == 0) {
             set_last_error(kErrorModNotFound);
         }
         c.ret(h);
     }},
    {"KERNEL32.dll", "GetModuleFileNameA", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t h = c.arg(0);
         if (h != 0 && h != process().image_base) {
             set_last_error(kErrorModNotFound);
             c.ret(0);
             return;
         }
         c.ret(copy_out_a(c.arg(1), c.arg(2), kGuestExePath));
     }},
    {"KERNEL32.dll", "LoadLibraryA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto name = mem::read_cstr(c.arg(0));
         uint32_t h = module_handle_for(name);
         std::fprintf(stderr, "[MODULE] LoadLibraryA(\"%s\")%s\n", name.c_str(), h ? "" : " -> not available");
         if (h == 0) {
             set_last_error(kErrorModNotFound);
         }
         c.ret(h);
     }},
    {"KERNEL32.dll", "FreeLibrary", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "GetProcAddress", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto dll = module_name_for(c.arg(0));
         uint32_t name = c.arg(1);
         if (dll.empty()) {
             set_last_error(kErrorProcNotFound);
             c.ret(0);
             return;
         }
         if (name < 0x10000) {
             c.ret(thunk_for(dll, "", static_cast<uint16_t>(name)));
         } else {
             auto n = mem::read_cstr(name);
             std::fprintf(stderr, "[MODULE] GetProcAddress(%s, \"%s\")\n", dll.c_str(), n.c_str());
             c.ret(thunk_for(dll, n, 0));
         }
     }},
    {"KERNEL32.dll", "HeapCreate", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(handle_insert(std::make_shared<HeapObject>())); }},
    {"KERNEL32.dll", "HeapDestroy", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(handle_close(c.arg(0)) ? 1 : 0); }},
    {"KERNEL32.dll", "HeapAlloc", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(allocator().alloc(std::max<uint32_t>(c.arg(2), 1))); }},
    {"KERNEL32.dll", "HeapFree", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t block = c.arg(2);
         if (block != 0 && !allocator().owns(block)) {
             std::string stack;
             for (uint32_t i = 0; i < 12; ++i) {
                 stack += std::format(" {:08x}", mem::read<uint32_t>(esp(c.s) + 4 * i));
             }
             std::fprintf(stderr, "[HEAP] HeapFree of foreign block 0x%08x, stack:%s\n", block, stack.c_str());
             set_last_error(87);
             c.ret(0);
             return;
         }
         allocator().free(block);
         c.ret(1);
     }},
    {"KERNEL32.dll", "HeapReAlloc", 0, 4, Conv::stdcall,
     [](ApiCall& c) { c.ret(allocator().realloc(c.arg(2), std::max<uint32_t>(c.arg(3), 1))); }},
    {"KERNEL32.dll", "HeapSize", 0, 3, Conv::stdcall, [](ApiCall& c) { c.ret(allocator().size_of(c.arg(2))); }},
    {"KERNEL32.dll", "LocalAlloc", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(allocator().alloc(std::max<uint32_t>(c.arg(1), 1))); }},
    {"KERNEL32.dll", "LocalFree", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         allocator().free(c.arg(0));
         c.ret(0);
     }},
    {"KERNEL32.dll", "VirtualAlloc", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t addr = c.arg(0);
         uint32_t size = c.arg(1);
         if (addr != 0) {
             c.ret(addr & ~0xfffu);
             return;
         }
         c.ret(allocator().reserve(size, 0x10000));
     }},
    {"KERNEL32.dll", "VirtualFree", 0, 3, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "VirtualProtect", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         if (c.arg(3) != 0) {
             mem::write<uint32_t>(c.arg(3), 0x04);
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "VirtualQuery", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t addr = c.arg(0);
         uint32_t p = c.arg(1);
         uint32_t page = addr & ~0xfffu;
         uint32_t base = page;
         auto* t = current_thread();
         if (t && addr >= t->stack_bottom && addr < t->stack_top) {
             base = t->stack_bottom;
         }
         mem::write<uint32_t>(p + 0, page);
         mem::write<uint32_t>(p + 4, base);
         mem::write<uint32_t>(p + 8, 0x04);
         mem::write<uint32_t>(p + 12, 0x1000);
         mem::write<uint32_t>(p + 16, 0x1000);
         mem::write<uint32_t>(p + 20, 0x04);
         mem::write<uint32_t>(p + 24, 0x20000);
         c.ret(28);
     }},
    {"KERNEL32.dll", "GetSystemInfo", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t p = c.arg(0);
         std::memset(mem::ptr(p), 0, 36);
         mem::write<uint32_t>(p + 4, 0x1000);
         mem::write<uint32_t>(p + 8, 0x00010000);
         mem::write<uint32_t>(p + 12, 0x7ffeffff);
         mem::write<uint32_t>(p + 16, 1);
         mem::write<uint32_t>(p + 20, 1);
         mem::write<uint32_t>(p + 24, 586);
         mem::write<uint32_t>(p + 28, 0x10000);
         mem::write<uint16_t>(p + 32, 6);
     }},
    {"KERNEL32.dll", "GlobalMemoryStatus", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t p = c.arg(0);
         mem::write<uint32_t>(p + 0, 32);
         mem::write<uint32_t>(p + 4, 25);
         mem::write<uint32_t>(p + 8, 0x7fffffff);
         mem::write<uint32_t>(p + 12, 0x60000000);
         mem::write<uint32_t>(p + 16, 0x7fffffff);
         mem::write<uint32_t>(p + 20, 0x60000000);
         mem::write<uint32_t>(p + 24, 0x7ffe0000);
         mem::write<uint32_t>(p + 28, 0x60000000);
     }},
    {"KERNEL32.dll", "TlsAlloc", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(tls_alloc()); }},
    {"KERNEL32.dll", "TlsFree", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(tls_free(c.arg(0)) ? 1 : 0); }},
    {"KERNEL32.dll", "TlsGetValue", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t slot = c.arg(0);
         auto* t = current_thread();
         set_last_error(0);
         c.ret(t && slot < kTlsSlots ? t->tls[slot] : 0);
     }},
    {"KERNEL32.dll", "TlsSetValue", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t slot = c.arg(0);
         auto* t = current_thread();
         if (!t || slot >= kTlsSlots) {
             c.ret(0);
             return;
         }
         t->tls[slot] = c.arg(1);
         c.ret(1);
     }},
    {"KERNEL32.dll", "InitializeCriticalSection", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::memset(mem::ptr(c.arg(0)), 0, 24);
         mem::write<int32_t>(c.arg(0) + 4, -1);
         critical_section(c.arg(0));
     }},
    {"KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::memset(mem::ptr(c.arg(0)), 0, 24);
         mem::write<int32_t>(c.arg(0) + 4, -1);
         critical_section(c.arg(0));
         c.ret(1);
     }},
    {"KERNEL32.dll", "DeleteCriticalSection", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(g_cs_mutex);
         g_critical_sections.erase(c.arg(0));
     }},
    {"KERNEL32.dll", "EnterCriticalSection", 0, 1, Conv::stdcall,
     [](ApiCall& c) { critical_section(c.arg(0)).lock(); }},
    {"KERNEL32.dll", "LeaveCriticalSection", 0, 1, Conv::stdcall,
     [](ApiCall& c) { critical_section(c.arg(0)).unlock(); }},
    {"KERNEL32.dll", "InterlockedIncrement", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto* p = reinterpret_cast<int32_t*>(mem::ptr(c.arg(0)));
         c.ret(static_cast<uint32_t>(__atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST)));
     }},
    {"KERNEL32.dll", "InterlockedDecrement", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto* p = reinterpret_cast<int32_t*>(mem::ptr(c.arg(0)));
         c.ret(static_cast<uint32_t>(__atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST)));
     }},
    {"KERNEL32.dll", "InterlockedExchange", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto* p = reinterpret_cast<uint32_t*>(mem::ptr(c.arg(0)));
         c.ret(__atomic_exchange_n(p, c.arg(1), __ATOMIC_SEQ_CST));
     }},
    {"KERNEL32.dll", "GetCurrentThreadId", 0, 0, Conv::stdcall,
     [](ApiCall& c) { c.ret(current_thread() ? current_thread()->id : 0); }},
    {"KERNEL32.dll", "GetCurrentProcessId", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0x1000); }},
    {"KERNEL32.dll", "GetCurrentThread", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0xfffffffe); }},
    {"KERNEL32.dll", "GetCurrentProcess", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0xffffffff); }},
    {"KERNEL32.dll", "GetLastError", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(last_error()); }},
    {"KERNEL32.dll", "SetLastError", 0, 1, Conv::stdcall, [](ApiCall& c) { set_last_error(c.arg(0)); }},
    {"KERNEL32.dll", "GetACP", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(1252); }},
    {"KERNEL32.dll", "GetOEMCP", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(437); }},
    {"KERNEL32.dll", "IsValidCodePage", 0, 1, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(0) == 1252 || c.arg(0) == 437 || c.arg(0) == 65001 ? 1 : 0); }},
    {"KERNEL32.dll", "GetCPInfo", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t p = c.arg(1);
         std::memset(mem::ptr(p), 0, 20);
         mem::write<uint32_t>(p, 1);
         mem::write<uint8_t>(p + 4, '?');
         c.ret(1);
     }},
    {"KERNEL32.dll", "MultiByteToWideChar", 0, 6, Conv::stdcall,
     [](ApiCall& c) { c.ret(multibyte_to_wide(c.arg(2), c.arg(3), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "WideCharToMultiByte", 0, 8, Conv::stdcall,
     [](ApiCall& c) { c.ret(wide_to_multibyte(c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(7))); }},
    {"KERNEL32.dll", "GetStringTypeA", 0, 5, Conv::stdcall,
     [](ApiCall& c) { c.ret(string_type_a(c.arg(1), c.arg(2), c.arg(3), c.arg(4))); }},
    {"KERNEL32.dll", "GetStringTypeW", 0, 4, Conv::stdcall,
     [](ApiCall& c) { c.ret(string_type_w(c.arg(0), c.arg(1), c.arg(2), c.arg(3))); }},
    {"KERNEL32.dll", "LCMapStringA", 0, 6, Conv::stdcall,
     [](ApiCall& c) { c.ret(lcmap_a(c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "LCMapStringW", 0, 6, Conv::stdcall,
     [](ApiCall& c) { c.ret(lcmap_w(c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "CompareStringA", 0, 6, Conv::stdcall,
     [](ApiCall& c) { c.ret(compare_a(c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "CompareStringW", 0, 6, Conv::stdcall,
     [](ApiCall& c) { c.ret(compare_w(c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "GetLocaleInfoA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t n = locale_info(c.arg(1), c.arg(2), c.arg(3), false);
         if (n == 0) {
             set_last_error(kErrorInvalidFlags);
         }
         c.ret(n);
     }},
    {"KERNEL32.dll", "GetLocaleInfoW", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t n = locale_info(c.arg(1), c.arg(2), c.arg(3), true);
         if (n == 0) {
             set_last_error(kErrorInvalidFlags);
         }
         c.ret(n);
     }},
    {"KERNEL32.dll", "GetUserDefaultLCID", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0x0409); }},
    {"KERNEL32.dll", "GetSystemDefaultLCID", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0x0409); }},
    {"KERNEL32.dll", "IsValidLocale", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(c.arg(0) == 0x0409 ? 1 : 0); }},
    {"KERNEL32.dll", "EnumSystemLocalesA", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "GetEnvironmentStrings", 0, 0, Conv::stdcall,
     [](ApiCall& c) { c.ret(process().environment_a); }},
    {"KERNEL32.dll", "GetEnvironmentStringsW", 0, 0, Conv::stdcall,
     [](ApiCall& c) { c.ret(process().environment_w); }},
    {"KERNEL32.dll", "FreeEnvironmentStringsA", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "FreeEnvironmentStringsW", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "SetEnvironmentVariableA", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "GetStdHandle", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         int32_t which = static_cast<int32_t>(c.arg(0));
         int index = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
         c.ret(index < 0 ? 0xffffffffu : g_std_handles[index]);
     }},
    {"KERNEL32.dll", "SetStdHandle", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "SetHandleCount", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(c.arg(0)); }},
    {"KERNEL32.dll", "FlushFileBuffers", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "CloseHandle", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(handle_close(c.arg(0)) ? 1 : 0); }},
    {"KERNEL32.dll", "GetSystemTimeAsFileTime", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         timeval tv;
         gettimeofday(&tv, nullptr);
         uint64_t ft = (static_cast<uint64_t>(tv.tv_sec) + 11644473600ull) * 10000000ull +
                       static_cast<uint64_t>(tv.tv_usec) * 10;
         mem::write<uint64_t>(c.arg(0), ft);
     }},
    {"KERNEL32.dll", "GetLocalTime", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         timeval tv;
         gettimeofday(&tv, nullptr);
         std::tm t{};
         localtime_r(&tv.tv_sec, &t);
         write_systemtime(c.arg(0), t, static_cast<int>(tv.tv_usec / 1000));
     }},
    {"KERNEL32.dll", "GetTimeZoneInformation", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::memset(mem::ptr(c.arg(0)), 0, 172);
         c.ret(0);
     }},
    {"KERNEL32.dll", "GetTickCount", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(tick_ms()); }},
    {"KERNEL32.dll", "QueryPerformanceCounter", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         mem::write<uint64_t>(c.arg(0), now_ns() / 100);
         c.ret(1);
     }},
    {"KERNEL32.dll", "QueryPerformanceFrequency", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         mem::write<uint64_t>(c.arg(0), 10000000ull);
         c.ret(1);
     }},
    {"KERNEL32.dll", "Sleep", 0, 1, Conv::stdcall, [](ApiCall& c) { usleep(c.arg(0) * 1000); }},
    {"KERNEL32.dll", "SetErrorMode", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"KERNEL32.dll", "SetConsoleCtrlHandler", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "IsProcessorFeaturePresent", 0, 1, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(0) == 6 || c.arg(0) == 10 ? 1 : 0); }},
    {"KERNEL32.dll", "IsBadReadPtr", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(0) < kNullGuardEnd && c.arg(1) != 0 ? 1 : 0); }},
    {"KERNEL32.dll", "IsBadWritePtr", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(0) < kNullGuardEnd && c.arg(1) != 0 ? 1 : 0); }},
    {"KERNEL32.dll", "IsBadCodePtr", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(c.arg(0) < kNullGuardEnd ? 1 : 0); }},
    {"KERNEL32.dll", "OutputDebugStringA", 0, 1, Conv::stdcall,
     [](ApiCall& c) { std::fprintf(stderr, "[DEBUG] %s\n", mem::read_cstr(c.arg(0)).c_str()); }},
    {"KERNEL32.dll", "FatalAppExitA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[FATAL] %s\n", mem::read_cstr(c.arg(1)).c_str());
         exit_process(1);
     }},
    {"KERNEL32.dll", "ExitProcess", 0, 1, Conv::stdcall, [](ApiCall& c) { exit_process(c.arg(0)); }},
    {"KERNEL32.dll", "TerminateProcess", 0, 2, Conv::stdcall, [](ApiCall& c) { exit_process(c.arg(1)); }},
    {"KERNEL32.dll", "GetSystemDirectoryA", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(copy_out_a(c.arg(0), c.arg(1), "C:\\WINDOWS\\system32")); }},
    {"KERNEL32.dll", "lstrlenA", 0, 1, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(0) ? static_cast<uint32_t>(mem::read_cstr(c.arg(0)).size()) : 0); }},
    {"KERNEL32.dll", "lstrcpyA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         mem::write_cstr(c.arg(0), mem::read_cstr(c.arg(1)));
         c.ret(c.arg(0));
     }},
    {"KERNEL32.dll", "lstrcatA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         mem::write_cstr(c.arg(0), mem::read_cstr(c.arg(0)) + mem::read_cstr(c.arg(1)));
         c.ret(c.arg(0));
     }},
    {"KERNEL32.dll", "lstrcmpiA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto a = mem::read_cstr(c.arg(0));
         auto b = mem::read_cstr(c.arg(1));
         c.ret(static_cast<uint32_t>(strcasecmp(a.c_str(), b.c_str())));
     }},
};

}

[[noreturn]] void exit_process(uint32_t code) {
    audio::finish_output();
    perf::finish();
    std::fprintf(stderr, "[EXIT] process exited with code %u\n", code);
    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(static_cast<int>(code));
}

std::span<const ApiDef> kernel32_apis() {
    static bool std_ready = [] {
        g_std_handles[0] = handle_insert(std::make_shared<StdStream>(stdin));
        g_std_handles[1] = handle_insert(std::make_shared<StdStream>(stdout));
        g_std_handles[2] = handle_insert(std::make_shared<StdStream>(stderr));
        return true;
    }();
    (void)std_ready;
    return kApis;
}

}
