#include "dshow.hpp"
#include "paths.hpp"
#include "process.hpp"

#include <mach/mach_time.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorMoreData = 234;
constexpr uint32_t kRegSz = 1;
constexpr uint32_t kFirstKeyHandle = 0x80001000;

struct RegValue {
    uint32_t type = kRegSz;
    std::vector<uint8_t> data;
};

struct Registry {
    std::mutex mutex;
    std::map<std::string, std::map<std::string, RegValue>> keys;
    std::map<uint32_t, std::string> open;
    uint32_t next = kFirstKeyHandle;
};

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

RegValue string_value(std::string_view text) {
    RegValue v;
    v.type = kRegSz;
    v.data.assign(text.begin(), text.end());
    v.data.push_back(0);
    return v;
}

Registry& registry() {
    static Registry r;
    static const bool seeded = [] {
        r.keys["hkcu\\software\\microsoft\\windows\\currentversion\\explorer\\shell folders"]["personal"] =
            string_value(kGuestDocuments);
        return true;
    }();
    (void)seeded;
    return r;
}

uint32_t time_ms() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    static uint64_t start = mach_absolute_time();
    uint64_t ns = static_cast<uint64_t>(static_cast<unsigned __int128>(mach_absolute_time() - start) * tb.numer /
                                        tb.denom);
    return static_cast<uint32_t>(ns / 1000000 + 0x10000);
}

std::string root_name(uint32_t root) {
    switch (root) {
    case 0x80000000: return "hkcr";
    case 0x80000001: return "hkcu";
    case 0x80000002: return "hklm";
    case 0x80000003: return "hku";
    default: return "";
    }
}

std::optional<std::string> key_path(uint32_t parent, uint32_t subkey) {
    auto& r = registry();
    std::string base = root_name(parent);
    if (base.empty()) {
        auto it = r.open.find(parent);
        if (it == r.open.end()) {
            return std::nullopt;
        }
        base = it->second;
    }
    std::string sub = subkey ? lower(mem::read_cstr(subkey)) : "";
    while (!sub.empty() && sub.back() == '\\') {
        sub.pop_back();
    }
    return sub.empty() ? base : base + "\\" + sub;
}

uint32_t open_key(uint32_t parent, uint32_t subkey, uint32_t out, bool create, const char* api) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto path = key_path(parent, subkey);
    if (!path) {
        return kErrorInvalidHandle;
    }
    bool exists = r.keys.contains(*path);
    std::fprintf(stderr, "[REGISTRY] %s(\"%s\") %s\n", api, path->c_str(), exists ? "found" : create ? "created" : "missing");
    if (!exists && !create) {
        return kErrorFileNotFound;
    }
    r.keys[*path];
    uint32_t handle = r.next;
    r.next += 4;
    r.open[handle] = *path;
    if (out != 0) {
        mem::write<uint32_t>(out, handle);
    }
    return kErrorSuccess;
}

constexpr ApiDef kAdvapi32[] = {
    {"ADVAPI32.dll", "RegOpenKeyA", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(open_key(c.arg(0), c.arg(1), c.arg(2), false, "RegOpenKeyA")); }},
    {"ADVAPI32.dll", "RegOpenKeyExA", 0, 5, Conv::stdcall,
     [](ApiCall& c) { c.ret(open_key(c.arg(0), c.arg(1), c.arg(4), false, "RegOpenKeyExA")); }},
    {"ADVAPI32.dll", "RegCreateKeyExA", 0, 9, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t rc = open_key(c.arg(0), c.arg(1), c.arg(7), true, "RegCreateKeyExA");
         if (rc == kErrorSuccess && c.arg(8) != 0) {
             mem::write<uint32_t>(c.arg(8), 1);
         }
         c.ret(rc);
     }},
    {"ADVAPI32.dll", "RegQueryValueExA", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         auto& r = registry();
         std::lock_guard lock(r.mutex);
         auto key = r.open.find(c.arg(0));
         if (key == r.open.end()) {
             c.ret(kErrorInvalidHandle);
             return;
         }
         std::string name = c.arg(1) ? lower(mem::read_cstr(c.arg(1))) : "";
         auto& values = r.keys[key->second];
         auto it = values.find(name);
         std::fprintf(stderr, "[REGISTRY] RegQueryValueExA(\"%s\", \"%s\") %s\n", key->second.c_str(), name.c_str(),
                      it == values.end() ? "missing" : "found");
         if (it == values.end()) {
             c.ret(kErrorFileNotFound);
             return;
         }
         if (c.arg(3) != 0) {
             mem::write<uint32_t>(c.arg(3), it->second.type);
         }
         uint32_t size = static_cast<uint32_t>(it->second.data.size());
         uint32_t capacity = c.arg(5) ? mem::read<uint32_t>(c.arg(5)) : 0;
         if (c.arg(5) != 0) {
             mem::write<uint32_t>(c.arg(5), size);
         }
         if (c.arg(4) == 0) {
             c.ret(kErrorSuccess);
             return;
         }
         if (capacity < size) {
             c.ret(kErrorMoreData);
             return;
         }
         std::memcpy(mem::ptr(c.arg(4)), it->second.data.data(), size);
         c.ret(kErrorSuccess);
     }},
    {"ADVAPI32.dll", "RegSetValueExA", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         auto& r = registry();
         std::lock_guard lock(r.mutex);
         auto key = r.open.find(c.arg(0));
         if (key == r.open.end()) {
             c.ret(kErrorInvalidHandle);
             return;
         }
         RegValue v;
         v.type = c.arg(3);
         const uint8_t* data = mem::ptr(c.arg(4));
         v.data.assign(data, data + c.arg(5));
         r.keys[key->second][c.arg(1) ? lower(mem::read_cstr(c.arg(1))) : ""] = std::move(v);
         c.ret(kErrorSuccess);
     }},
    {"ADVAPI32.dll", "RegCloseKey", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto& r = registry();
         std::lock_guard lock(r.mutex);
         c.ret(r.open.erase(c.arg(0)) ? kErrorSuccess : kErrorInvalidHandle);
     }},
};

constexpr ApiDef kWinmm[] = {
    {"WINMM.dll", "timeGetTime", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(time_ms()); }},
    {"WINMM.dll", "timeBeginPeriod", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"WINMM.dll", "timeEndPeriod", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"WINMM.dll", "timeGetDevCaps", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         mem::write<uint32_t>(c.arg(0), 1);
         mem::write<uint32_t>(c.arg(0) + 4, 1000000);
         c.ret(0);
     }},
};

std::string guid_text(uint32_t addr) {
    if (addr == 0) {
        return "null";
    }
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                       mem::read<uint32_t>(addr), mem::read<uint16_t>(addr + 4), mem::read<uint16_t>(addr + 6),
                       mem::read<uint8_t>(addr + 8), mem::read<uint8_t>(addr + 9), mem::read<uint8_t>(addr + 10),
                       mem::read<uint8_t>(addr + 11), mem::read<uint8_t>(addr + 12), mem::read<uint8_t>(addr + 13),
                       mem::read<uint8_t>(addr + 14), mem::read<uint8_t>(addr + 15));
}

constexpr uint32_t kClassNotRegistered = 0x80040154u;

constexpr ApiDef kOle32[] = {
    {"ole32.dll", "CoInitialize", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"ole32.dll", "CoUninitialize", 0, 0, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"ole32.dll", "CoCreateInstance", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         if (is_filter_graph(c.arg(0))) {
             c.ret(create_filter_graph(c.arg(3), c.arg(4)));
             return;
         }
         std::fprintf(stderr, "[COM] CoCreateInstance(clsid=%s, iid=%s) not available\n", guid_text(c.arg(0)).c_str(),
                      guid_text(c.arg(3)).c_str());
         if (c.arg(4) != 0) {
             mem::write<uint32_t>(c.arg(4), 0);
         }
         c.ret(kClassNotRegistered);
     }},
};

}

std::span<const ApiDef> ole32_apis() { return kOle32; }
std::span<const ApiDef> advapi32_apis() { return kAdvapi32; }
std::span<const ApiDef> winmm_apis() { return kWinmm; }

}
