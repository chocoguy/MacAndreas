#include "process.hpp"
#include "user32.hpp"
#include "wsprintf.hpp"

#include "../dispatch.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <string>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kWmCreate = 0x0001;
constexpr uint32_t kWmDestroy = 0x0002;
constexpr uint32_t kWmSetText = 0x000c;
constexpr uint32_t kWmShowWindow = 0x0018;
constexpr uint32_t kWmNcCreate = 0x0081;
constexpr uint32_t kWmNcDestroy = 0x0082;

constexpr int32_t kGwlWndProc = -4;
constexpr int32_t kGwlHInstance = -6;
constexpr int32_t kGwlStyle = -16;
constexpr int32_t kGwlExStyle = -20;
constexpr int32_t kGwlUserData = -21;

constexpr uint32_t kSpiGetScreenSaveActive = 0x0010;
constexpr uint32_t kSpiGetWorkArea = 0x0030;
constexpr uint32_t kSpiGetForegroundLockTimeout = 0x2000;
constexpr uint32_t kSpiSetForegroundLockTimeout = 0x2001;
constexpr uint32_t kSpiSetScreenSaveActive = 0x0011;

constexpr uint32_t kFirstHwnd = 0x00c00010;
constexpr uint32_t kFirstAtom = 0xc000;
constexpr uint32_t kIconBase = 0x00b10000;
constexpr uint32_t kCursorBase = 0x00b20000;

struct WindowClass {
    std::string name;
    uint32_t atom = 0;
    uint32_t style = 0;
    uint32_t wndproc = 0;
    uint32_t cls_extra = 0;
    uint32_t wnd_extra = 0;
    uint32_t instance = 0;
    uint32_t icon = 0;
    uint32_t cursor = 0;
    uint32_t background = 0;
};

struct Registry {
    std::recursive_mutex mutex;
    std::map<std::string, WindowClass> classes;
    std::map<uint32_t, Window> windows;
    uint32_t next_hwnd = kFirstHwnd;
    uint32_t next_atom = kFirstAtom;
    uint32_t focus = 0;
    uint32_t capture = 0;
    uint32_t foreground = 0;
    int32_t cursor_count = 0;
    uint32_t cursor = 0;
    uint32_t next_timer = 1;
};

Registry& registry() {
    static Registry r;
    return r;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string class_name_arg(uint32_t value) {
    if (value != 0 && value < 0x10000) {
        auto& r = registry();
        for (auto& [key, cls] : r.classes) {
            if (cls.atom == value) {
                return key;
            }
        }
        return std::format("#{}", value);
    }
    return lower(mem::read_cstr(value));
}

Window* find_window(uint32_t hwnd) {
    auto& r = registry();
    auto it = r.windows.find(hwnd);
    return it == r.windows.end() ? nullptr : &it->second;
}

void write_rect(uint32_t addr, int32_t left, int32_t top, int32_t right, int32_t bottom) {
    mem::write<int32_t>(addr, left);
    mem::write<int32_t>(addr + 4, top);
    mem::write<int32_t>(addr + 8, right);
    mem::write<int32_t>(addr + 12, bottom);
}

uint32_t send_to(GuestState& s, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    uint32_t proc = 0;
    {
        std::lock_guard lock(registry().mutex);
        if (Window* w = find_window(hwnd)) {
            proc = w->wndproc;
        }
    }
    if (proc == 0) {
        return 0;
    }
    return call_guest(s, proc, {hwnd, msg, wparam, lparam}, true);
}

uint32_t create_window(GuestState& s, uint32_t ex_style, uint32_t class_arg, uint32_t title, uint32_t style, int32_t x,
                       int32_t y, int32_t width, int32_t height, uint32_t parent, uint32_t menu, uint32_t instance,
                       uint32_t param) {
    auto& r = registry();
    std::string cls_name = class_name_arg(class_arg);
    Window w;
    {
        std::lock_guard lock(r.mutex);
        auto it = r.classes.find(cls_name);
        if (it == r.classes.end()) {
            std::fprintf(stderr, "[WINDOW] CreateWindowExA: class \"%s\" is not registered\n", cls_name.c_str());
            set_last_error(1407);
            return 0;
        }
        w.hwnd = r.next_hwnd;
        r.next_hwnd += 4;
        w.class_name = cls_name;
        w.wndproc = it->second.wndproc;
        w.instance = instance;
        w.title = title ? mem::read_cstr(title) : "";
        w.style = style;
        w.ex_style = ex_style;
        w.parent = parent;
        w.x = x == static_cast<int32_t>(0x80000000) ? 0 : x;
        w.y = y == static_cast<int32_t>(0x80000000) ? 0 : y;
        auto screen = screen_size();
        w.width = width == static_cast<int32_t>(0x80000000) ? static_cast<int32_t>(screen.first) : width;
        w.height = height == static_cast<int32_t>(0x80000000) ? static_cast<int32_t>(screen.second) : height;
        r.windows[w.hwnd] = w;
    }
    std::fprintf(stderr, "[WINDOW] CreateWindowExA(class=\"%s\", title=\"%s\", style=0x%08x, ex=0x%08x, %d,%d %dx%d) -> 0x%08x\n",
                 cls_name.c_str(), w.title.c_str(), style, ex_style, w.x, w.y, w.width, w.height, w.hwnd);

    uint32_t cs = allocator().alloc(48);
    mem::write<uint32_t>(cs + 0, param);
    mem::write<uint32_t>(cs + 4, instance);
    mem::write<uint32_t>(cs + 8, menu);
    mem::write<uint32_t>(cs + 12, parent);
    mem::write<int32_t>(cs + 16, w.height);
    mem::write<int32_t>(cs + 20, w.width);
    mem::write<int32_t>(cs + 24, w.y);
    mem::write<int32_t>(cs + 28, w.x);
    mem::write<uint32_t>(cs + 32, style);
    mem::write<uint32_t>(cs + 36, title);
    mem::write<uint32_t>(cs + 40, class_arg);
    mem::write<uint32_t>(cs + 44, ex_style);

    uint32_t ok = send_to(s, w.hwnd, kWmNcCreate, 0, cs);
    if (ok == 0) {
        std::fprintf(stderr, "[WINDOW] WM_NCCREATE rejected window 0x%08x\n", w.hwnd);
        allocator().free(cs);
        std::lock_guard lock(r.mutex);
        r.windows.erase(w.hwnd);
        return 0;
    }
    int32_t created = static_cast<int32_t>(send_to(s, w.hwnd, kWmCreate, 0, cs));
    allocator().free(cs);
    if (created == -1) {
        std::fprintf(stderr, "[WINDOW] WM_CREATE rejected window 0x%08x\n", w.hwnd);
        std::lock_guard lock(r.mutex);
        r.windows.erase(w.hwnd);
        return 0;
    }
    return w.hwnd;
}

uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    switch (msg) {
    case kWmNcCreate:
        return 1;
    case kWmSetText: {
        std::lock_guard lock(registry().mutex);
        if (Window* w = find_window(hwnd)) {
            w->title = lparam ? mem::read_cstr(lparam) : "";
        }
        return 1;
    }
    default:
        return 0;
    }
}

uint32_t get_window_long(uint32_t hwnd, int32_t index) {
    std::lock_guard lock(registry().mutex);
    Window* w = find_window(hwnd);
    if (!w) {
        set_last_error(1400);
        return 0;
    }
    switch (index) {
    case kGwlWndProc:
        return w->wndproc;
    case kGwlHInstance:
        return w->instance;
    case kGwlStyle:
        return w->style;
    case kGwlExStyle:
        return w->ex_style;
    case kGwlUserData:
        return w->user_data;
    default: {
        auto it = w->extra.find(index);
        return it == w->extra.end() ? 0 : it->second;
    }
    }
}

uint32_t set_window_long(uint32_t hwnd, int32_t index, uint32_t value) {
    std::lock_guard lock(registry().mutex);
    Window* w = find_window(hwnd);
    if (!w) {
        set_last_error(1400);
        return 0;
    }
    uint32_t* slot = nullptr;
    switch (index) {
    case kGwlWndProc:
        slot = &w->wndproc;
        break;
    case kGwlHInstance:
        slot = &w->instance;
        break;
    case kGwlStyle:
        slot = &w->style;
        break;
    case kGwlExStyle:
        slot = &w->ex_style;
        break;
    case kGwlUserData:
        slot = &w->user_data;
        break;
    default:
        slot = &w->extra[index];
        break;
    }
    uint32_t old = *slot;
    *slot = value;
    return old;
}

constexpr ApiDef kUser32[] = {
    {"USER32.dll", "GetKeyboardLayout", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0x04090409); }},
    {"USER32.dll", "SystemParametersInfoA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t action = c.arg(0);
         uint32_t out = c.arg(2);
         switch (action) {
         case kSpiGetForegroundLockTimeout:
             if (out) {
                 mem::write<uint32_t>(out, 200000);
             }
             break;
         case kSpiGetScreenSaveActive:
             if (out) {
                 mem::write<uint32_t>(out, 0);
             }
             break;
         case kSpiGetWorkArea:
             if (out) {
                 auto screen = screen_size();
                 write_rect(out, 0, 0, static_cast<int32_t>(screen.first), static_cast<int32_t>(screen.second));
             }
             break;
         case kSpiSetForegroundLockTimeout:
         case kSpiSetScreenSaveActive:
             break;
         default:
             std::fprintf(stderr, "[USER32] SystemParametersInfoA(0x%04x, %u, 0x%08x) ignored\n", action, c.arg(1),
                          out);
             break;
         }
         c.ret(1);
     }},
    {"USER32.dll", "FindWindowA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::string cls = c.arg(0) ? class_name_arg(c.arg(0)) : "";
         std::string title = c.arg(1) ? mem::read_cstr(c.arg(1)) : "";
         std::lock_guard lock(registry().mutex);
         for (auto& [hwnd, w] : registry().windows) {
             if ((cls.empty() || w.class_name == cls) && (title.empty() || w.title == title)) {
                 c.ret(hwnd);
                 return;
             }
         }
         c.ret(0);
     }},
    {"USER32.dll", "LoadIconA", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(kIconBase + (c.arg(1) < 0x10000 ? c.arg(1) : 0xffff)); }},
    {"USER32.dll", "LoadCursorA", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(kCursorBase + (c.arg(1) < 0x10000 ? c.arg(1) : 0xffff)); }},
    {"USER32.dll", "RegisterClassA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t wc = c.arg(0);
         WindowClass cls;
         cls.style = mem::read<uint32_t>(wc + 0);
         cls.wndproc = mem::read<uint32_t>(wc + 4);
         cls.cls_extra = mem::read<uint32_t>(wc + 8);
         cls.wnd_extra = mem::read<uint32_t>(wc + 12);
         cls.instance = mem::read<uint32_t>(wc + 16);
         cls.icon = mem::read<uint32_t>(wc + 20);
         cls.cursor = mem::read<uint32_t>(wc + 24);
         cls.background = mem::read<uint32_t>(wc + 28);
         uint32_t name_arg = mem::read<uint32_t>(wc + 36);
         cls.name = lower(mem::read_cstr(name_arg));
         auto& r = registry();
         std::lock_guard lock(r.mutex);
         if (r.classes.contains(cls.name)) {
             set_last_error(1410);
             c.ret(0);
             return;
         }
         cls.atom = r.next_atom++;
         std::fprintf(stderr, "[WINDOW] RegisterClassA(\"%s\", wndproc=0x%08x) -> 0x%04x\n", cls.name.c_str(),
                      cls.wndproc, cls.atom);
         uint32_t atom = cls.atom;
         r.classes[cls.name] = std::move(cls);
         c.ret(atom);
     }},
    {"USER32.dll", "AdjustWindowRect", 0, 3, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"USER32.dll", "AdjustWindowRectEx", 0, 4, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"USER32.dll", "CreateWindowExA", 0, 12, Conv::stdcall,
     [](ApiCall& c) {
         c.ret(create_window(c.s, c.arg(0), c.arg(1), c.arg(2), c.arg(3), static_cast<int32_t>(c.arg(4)),
                             static_cast<int32_t>(c.arg(5)), static_cast<int32_t>(c.arg(6)),
                             static_cast<int32_t>(c.arg(7)), c.arg(8), c.arg(9), c.arg(10), c.arg(11)));
     }},
    {"USER32.dll", "DestroyWindow", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t hwnd = c.arg(0);
         if (!find_window(hwnd)) {
             c.ret(0);
             return;
         }
         send_to(c.s, hwnd, kWmDestroy, 0, 0);
         send_to(c.s, hwnd, kWmNcDestroy, 0, 0);
         std::lock_guard lock(registry().mutex);
         registry().windows.erase(hwnd);
         c.ret(1);
     }},
    {"USER32.dll", "DefWindowProcA", 0, 4, Conv::stdcall,
     [](ApiCall& c) { c.ret(def_window_proc(c.arg(0), c.arg(1), c.arg(2), c.arg(3))); }},
    {"USER32.dll", "SendMessageA", 0, 4, Conv::stdcall,
     [](ApiCall& c) { c.ret(send_to(c.s, c.arg(0), c.arg(1), c.arg(2), c.arg(3))); }},
    {"USER32.dll", "ShowWindow", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t hwnd = c.arg(0);
         bool was_visible = false;
         bool show = c.arg(1) != 0;
         {
             std::lock_guard lock(registry().mutex);
             Window* w = find_window(hwnd);
             if (!w) {
                 c.ret(0);
                 return;
             }
             was_visible = w->visible;
             w->visible = show;
         }
         if (was_visible != show) {
             send_to(c.s, hwnd, kWmShowWindow, show ? 1 : 0, 0);
         }
         c.ret(was_visible ? 1 : 0);
     }},
    {"USER32.dll", "UpdateWindow", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(find_window(c.arg(0)) ? 1 : 0); }},
    {"USER32.dll", "SetFocus", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         uint32_t old = registry().focus;
         registry().focus = c.arg(0);
         c.ret(old);
     }},
    {"USER32.dll", "SetForegroundWindow", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         registry().foreground = c.arg(0);
         c.ret(1);
     }},
    {"USER32.dll", "GetWindowLongA", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(get_window_long(c.arg(0), static_cast<int32_t>(c.arg(1)))); }},
    {"USER32.dll", "SetWindowLongA", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(set_window_long(c.arg(0), static_cast<int32_t>(c.arg(1)), c.arg(2))); }},
    {"USER32.dll", "GetWindowRect", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         if (!w) {
             c.ret(0);
             return;
         }
         write_rect(c.arg(1), w->x, w->y, w->x + w->width, w->y + w->height);
         c.ret(1);
     }},
    {"USER32.dll", "GetClientRect", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         if (!w) {
             c.ret(0);
             return;
         }
         write_rect(c.arg(1), 0, 0, w->width, w->height);
         c.ret(1);
     }},
    {"USER32.dll", "ClientToScreen", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         if (!w) {
             c.ret(0);
             return;
         }
         mem::write<int32_t>(c.arg(1), mem::read<int32_t>(c.arg(1)) + w->x);
         mem::write<int32_t>(c.arg(1) + 4, mem::read<int32_t>(c.arg(1) + 4) + w->y);
         c.ret(1);
     }},
    {"USER32.dll", "SetWindowPos", 0, 7, Conv::stdcall,
     [](ApiCall& c) {
         constexpr uint32_t kNoSize = 0x0001;
         constexpr uint32_t kNoMove = 0x0002;
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         if (!w) {
             c.ret(0);
             return;
         }
         uint32_t flags = c.arg(6);
         if (!(flags & kNoMove)) {
             w->x = static_cast<int32_t>(c.arg(2));
             w->y = static_cast<int32_t>(c.arg(3));
         }
         if (!(flags & kNoSize)) {
             w->width = static_cast<int32_t>(c.arg(4));
             w->height = static_cast<int32_t>(c.arg(5));
         }
         c.ret(1);
     }},
    {"USER32.dll", "GetWindowPlacement", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         uint32_t wp = c.arg(1);
         if (!w || wp == 0) {
             c.ret(0);
             return;
         }
         mem::write<uint32_t>(wp + 4, 0);
         mem::write<uint32_t>(wp + 8, w->visible ? 1 : 0);
         write_rect(wp + 28, w->x, w->y, w->x + w->width, w->y + w->height);
         c.ret(1);
     }},
    {"USER32.dll", "SetWindowTextA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         Window* w = find_window(c.arg(0));
         if (!w) {
             c.ret(0);
             return;
         }
         w->title = c.arg(1) ? mem::read_cstr(c.arg(1)) : "";
         c.ret(1);
     }},
    {"USER32.dll", "IsIconic", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "GetMenu", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "ShowCursor", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         registry().cursor_count += c.arg(0) ? 1 : -1;
         c.ret(static_cast<uint32_t>(registry().cursor_count));
     }},
    {"USER32.dll", "SetCursor", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         uint32_t old = registry().cursor;
         registry().cursor = c.arg(0);
         c.ret(old);
     }},
    {"USER32.dll", "SetCursorPos", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"USER32.dll", "ClipCursor", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"USER32.dll", "SetCapture", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         uint32_t old = registry().capture;
         registry().capture = c.arg(0);
         c.ret(old);
     }},
    {"USER32.dll", "ReleaseCapture", 0, 0, Conv::stdcall,
     [](ApiCall& c) {
         std::lock_guard lock(registry().mutex);
         registry().capture = 0;
         c.ret(1);
     }},
    {"USER32.dll", "GetKeyState", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "MapVirtualKeyA", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(map_virtual_key(c.arg(0), c.arg(1))); }},
    {"USER32.dll", "SetTimer", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[USER32] SetTimer(0x%08x, %u, %u ms, 0x%08x) not delivered yet\n", c.arg(0), c.arg(1),
                      c.arg(2), c.arg(3));
         std::lock_guard lock(registry().mutex);
         c.ret(c.arg(1) ? c.arg(1) : registry().next_timer++);
     }},
    {"USER32.dll", "PeekMessageA", 0, 5, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "TranslateMessage", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "DispatchMessageA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t msg = c.arg(0);
         c.ret(send_to(c.s, mem::read<uint32_t>(msg), mem::read<uint32_t>(msg + 4), mem::read<uint32_t>(msg + 8),
                       mem::read<uint32_t>(msg + 12)));
     }},
    {"USER32.dll", "PostQuitMessage", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[USER32] PostQuitMessage(%d)\n", static_cast<int32_t>(c.arg(0)));
         c.ret(0);
     }},
    {"USER32.dll", "MessageBoxA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[MESSAGEBOX] %s: %s\n", c.arg(2) ? mem::read_cstr(c.arg(2)).c_str() : "",
                      c.arg(1) ? mem::read_cstr(c.arg(1)).c_str() : "");
         c.ret(1);
     }},
    {"USER32.dll", "DialogBoxParamA", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[USER32] DialogBoxParamA(template=0x%08x) not supported\n", c.arg(1));
         c.ret(static_cast<uint32_t>(-1));
     }},
    {"USER32.dll", "EndDialog", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"USER32.dll", "GetDlgItem", 0, 2, Conv::stdcall, [](ApiCall& c) { c.ret(0); }},
    {"USER32.dll", "wsprintfA", 0, 2, Conv::cdecl_,
     [](ApiCall& c) {
         auto text = format_wsprintf(mem::read_cstr(c.arg(1)), [&](unsigned i) { return c.arg(2 + i); });
         if (text.size() > 1024) {
             text.resize(1024);
         }
         mem::write_cstr(c.arg(0), text);
         c.ret(static_cast<uint32_t>(text.size()));
     }},
};

}

std::pair<uint32_t, uint32_t> screen_size() {
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    uint32_t width = static_cast<uint32_t>(bounds.size.width);
    uint32_t height = static_cast<uint32_t>(bounds.size.height);
    if (width == 0 || height == 0) {
        return {1920, 1080};
    }
    return {width, height};
}

std::vector<Window> windows_snapshot() {
    std::lock_guard lock(registry().mutex);
    std::vector<Window> out;
    for (auto& [hwnd, w] : registry().windows) {
        out.push_back(w);
    }
    return out;
}

std::span<const ApiDef> user32_apis() { return kUser32; }

}
