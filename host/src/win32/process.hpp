#pragma once

#include "../thunks.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ma::host::win32 {

inline constexpr std::string_view kGuestGameRoot = "C:\\GTA San Andreas";
inline constexpr std::string_view kGuestExePath = "C:\\GTA San Andreas\\gta_sa.exe";

struct ProcessInfo {
    std::filesystem::path game_dir;
    std::filesystem::path user_dir;
    uint32_t image_base = 0;
    uint32_t process_heap = 0;
    uint32_t command_line_a = 0;
    uint32_t environment_a = 0;
    uint32_t environment_w = 0;
};

ProcessInfo& process();

struct KObject {
    virtual ~KObject() = default;
};

struct StdStream : KObject {
    FILE* stream;
    explicit StdStream(FILE* s) : stream(s) {}
};

uint32_t handle_insert(std::shared_ptr<KObject> object);
std::shared_ptr<KObject> handle_lookup(uint32_t handle);
bool handle_close(uint32_t handle);

template <typename T>
std::shared_ptr<T> handle_get(uint32_t handle) {
    return std::dynamic_pointer_cast<T>(handle_lookup(handle));
}

[[noreturn]] void exit_process(uint32_t code);
void set_last_error(uint32_t code);
uint32_t last_error();

uint32_t module_handle_for(std::string_view dll);
std::string module_name_for(uint32_t handle);

void init_process(const std::filesystem::path& game_dir, uint32_t image_base);
void register_all();

std::span<const ApiDef> kernel32_apis();
std::span<const ApiDef> user32_apis();
std::span<const ApiDef> advapi32_apis();
std::span<const ApiDef> winmm_apis();
std::span<const ApiDef> sync_apis();
std::span<const ApiDef> seh_apis();
std::span<const ApiDef> file_apis();
std::span<const ApiDef> ddraw_apis();
std::span<const ApiDef> ole32_apis();
std::span<const ApiDef> dsound_apis();

}
