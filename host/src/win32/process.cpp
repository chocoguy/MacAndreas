#include "process.hpp"

#include "../settings.hpp"

#include "../gfx/d3d9.hpp"
#include "../input/input_state.hpp"
#include "../platform/window.hpp"
#include "../thread.hpp"
#include "user32.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

namespace ma::host::win32 {

namespace {

std::mutex g_handles_mutex;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x100;

std::mutex g_modules_mutex;
std::map<std::string, uint32_t> g_modules;

std::string lower_dll(std::string_view dll) {
    std::string out(dll);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto slash = out.find_last_of("\\/"); slash != std::string::npos) {
        out = out.substr(slash + 1);
    }
    if (!out.ends_with(".dll")) {
        out += ".dll";
    }
    return out;
}

}

ProcessInfo& process() {
    static ProcessInfo info;
    return info;
}

uint32_t handle_insert(std::shared_ptr<KObject> object) {
    std::lock_guard lock(g_handles_mutex);
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(object);
    return h;
}

std::shared_ptr<KObject> handle_lookup(uint32_t handle) {
    std::lock_guard lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool handle_close(uint32_t handle) {
    std::lock_guard lock(g_handles_mutex);
    return g_handles.erase(handle) != 0;
}

void set_last_error(uint32_t code) {
    if (auto* t = current_thread()) {
        t->last_error = code;
        mem::write<uint32_t>(t->teb + 0x34, code);
    }
}

uint32_t last_error() {
    auto* t = current_thread();
    return t ? mem::read<uint32_t>(t->teb + 0x34) : 0;
}

bool is_native_module(std::string_view dll) {
    static constexpr std::string_view kModules[] = {
        "kernel32.dll", "user32.dll",  "gdi32.dll", "advapi32.dll", "winmm.dll",  "ole32.dll",
        "ws2_32.dll",   "wsock32.dll", "eax.dll",   "vorbisfile.dll", "d3d9.dll", "dinput8.dll",
        "ddraw.dll",    "dsound.dll",
    };
    auto name = lower_dll(dll);
    return std::ranges::find(kModules, name) != std::end(kModules);
}

uint32_t module_handle_for(std::string_view dll) {
    std::lock_guard lock(g_modules_mutex);
    auto name = lower_dll(dll);
    if (auto it = g_modules.find(name); it != g_modules.end()) {
        return it->second;
    }
    if (!is_native_module(dll)) {
        return 0;
    }
    uint32_t h = 0x7c000000 + static_cast<uint32_t>(g_modules.size()) * 0x100000;
    g_modules[name] = h;
    return h;
}

std::string module_name_for(uint32_t handle) {
    std::lock_guard lock(g_modules_mutex);
    for (const auto& [name, h] : g_modules) {
        if (h == handle) {
            return name;
        }
    }
    return {};
}

void init_process(const std::filesystem::path& game_dir, uint32_t image_base) {
    auto& p = process();
    p.game_dir = game_dir;
    p.image_base = image_base;
    const char* home = std::getenv("HOME");
    p.user_dir = std::filesystem::path(home ? home : "/tmp") / "Library" / "Application Support" / "MacAndreas";
    std::error_code ec;
    std::filesystem::create_directories(p.user_dir / "Documents", ec);
    std::string cmd = std::string("\"") + std::string(kGuestExePath) + "\"";
    p.command_line_a = allocator().alloc(static_cast<uint32_t>(cmd.size() + 1));
    mem::write_cstr(p.command_line_a, cmd);
    p.environment_a = allocator().alloc(4);
    p.environment_w = allocator().alloc(8);
}

void register_all() {
    register_apis(kernel32_apis());
    register_apis(user32_apis());
    register_apis(advapi32_apis());
    register_apis(winmm_apis());
    register_apis(sync_apis());
    register_apis(seh_apis());
    register_apis(file_apis());
    register_apis(ddraw_apis());
    register_apis(ole32_apis());
    register_apis(dsound_apis());
    register_apis(dinput8_apis());
    register_apis(vorbisfile_apis());
    register_apis(d3d9::apis());
    platform::set_post_callback(post_message);
    platform::set_input_callbacks({key_event, mouse_move, mouse_button, mouse_wheel, [](uint32_t hwnd, bool active) {
                                       if (active || !settings().ignore_focus_loss) {
                                           focus_event(hwnd, active);
                                       }
                                   }});
    input::set_script_sink({
        [](const input::Key& key, bool down) { key_event(0, key, down, false); },
        [](int32_t x, int32_t y) {
            auto [ox, oy] = input::cursor();
            input::add_mouse_motion(x - ox, y - oy);
            mouse_move(0, x, y);
        },
        [](unsigned button, bool down) { mouse_button(0, button, down); },
        [](int32_t delta) { mouse_wheel(0, delta); },
        [](bool active) { focus_event(0, active); },
    });
}

}
