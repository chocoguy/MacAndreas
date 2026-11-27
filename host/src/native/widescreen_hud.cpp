#include "natives.hpp"

#include "../dispatch.hpp"
#include "../gfx/d3d9.hpp"
#include "../guest_memory.hpp"
#include "../manifest_runtime.hpp"
#include "../mod_settings.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ma::host::native {

namespace {

constexpr uint32_t kRsGlobalWidth = 0x00c17044;
constexpr uint32_t kRsGlobalHeight = 0x00c17048;
constexpr uint32_t kResXInv = 0x00859520;
constexpr uint32_t kDrawFonts = 0x0071a210;
constexpr uint32_t kGetStringWidth = 0x0071a0e0;
constexpr uint32_t kGetTextString = 0x006a0050;
constexpr uint32_t kInsertNumbers = 0x0069de90;
constexpr uint32_t kTheText = 0x00c1b340;
constexpr uint32_t kScriptTexts = 0x00a913e8;
constexpr uint32_t kScriptTextSize = 0x44;
constexpr size_t kScriptTextCount = 96;
constexpr uint32_t kScriptRects = 0x00a92d68;
constexpr uint32_t kScriptRectSize = 0x3c;
constexpr size_t kScriptRectCount = 128;
constexpr uint32_t kScriptTextsUsed = 0x00a44b68;
constexpr uint32_t kScriptRectsUsed = 0x00a44b5c;
constexpr uint32_t kHelpTextScaleOperand = 0x0058bffe;
constexpr uint32_t kScriptMenus = 0x00ba82d8;
constexpr uint32_t kScriptMenuX = 0x40c;
constexpr uint32_t kMenuManager = 0x00ba6748;
constexpr uint32_t kDrawingMap = 0x59;
constexpr uint32_t kScriptMenuA = 0x00580e00;
constexpr uint32_t kScriptMenuB = 0x005816e0;
constexpr uint32_t kMenuActive = 0x5c;
constexpr uint32_t kMouseFields[] = {0xbc, 0xe0, 0x1af8};
constexpr uint32_t kMouseRawX = 0xe0;
constexpr uint32_t kMenuDraw = 0x0057c290;
constexpr uint32_t kDrawRect = 0x00727b60;
constexpr uint32_t kRwEngineInstance = 0x00c97b24;
constexpr uint32_t kCurrentSplash = 0x008d093c;
constexpr uint32_t kSplashSprites = 0x00bab35c;
constexpr uint32_t kSplashCount = 7;
constexpr uint32_t kTextureName = 0x10;
constexpr uint32_t kRasterWidth = 0x0c;
constexpr uint32_t kRasterHeight = 0x10;
constexpr uint32_t kCurrentScreen = 0x15d;
constexpr uint8_t kScreenMap = 5;
constexpr uint32_t kMapZoom = 0x64;
constexpr uint32_t kMapOriginX = 0x68;
constexpr uint32_t kMapOriginY = 0x6c;
constexpr uint32_t kMapMouse = 0x70;
constexpr uint32_t kMapLoaded = 0x78;
constexpr uint32_t kDrawAspectRatio = 0x00c3efa4;
constexpr uint32_t kMapTransformPoint = 0x00583530;
constexpr uint32_t kMapInitResume = 0x005751ab;
constexpr uint32_t kMapClampResume = 0x00578821;
constexpr float kDefaultAspect = 4.0f / 3.0f;

namespace text {
constexpr uint32_t scale_x = 0x00;
constexpr uint32_t justify = 0x0c;
constexpr uint32_t centered = 0x0d;
constexpr uint32_t wrap_x = 0x10;
constexpr uint32_t before_fade = 0x23;
constexpr uint32_t right_justify = 0x24;
constexpr uint32_t x = 0x2c;
constexpr uint32_t y = 0x30;
constexpr uint32_t key = 0x34;
constexpr uint32_t numbers = 0x3c;
}

namespace rect {
constexpr uint32_t type = 0x00;
constexpr uint32_t before_fade = 0x04;
constexpr uint32_t ax = 0x08;
constexpr uint32_t ay = 0x0c;
constexpr uint32_t bx = 0x10;
constexpr uint32_t by = 0x14;
}

enum class Layout : uint8_t { center, left_panels, right_panels, window_panels, table };

std::array<Layout, kScriptTextCount> g_text_layouts{};
std::array<Layout, kScriptRectCount> g_rect_layouts{};
std::array<uint32_t, kScriptTextCount> g_text_owners{};
std::array<uint32_t, kScriptRectCount> g_rect_owners{};
uint32_t g_last_script = 0;
size_t g_text_mark = 0;
size_t g_rect_mark = 0;

float screen_width() { return static_cast<float>(mem::read<int32_t>(kRsGlobalWidth)); }

float screen_height() { return static_cast<float>(mem::read<int32_t>(kRsGlobalHeight)); }

float screen_aspect() {
    float h = screen_height();
    return h > 0.0f ? screen_width() / h : kDefaultAspect;
}

float scale_x(float x) { return x * screen_width() / 640.0f * kDefaultAspect / screen_aspect(); }

float offset_43() { return (screen_height() * kDefaultAspect - screen_width()) / 2.0f; }

float aspect_setting(const char* key) {
    std::string v = mod_settings().choice(key);
    auto colon = v.find(':');
    if (colon == std::string::npos) {
        return 0.0f;
    }
    return std::stof(v.substr(0, colon)) / std::stof(v.substr(colon + 1));
}

float hud_offset() {
    static const float aspect = aspect_setting("widescreen.hud_aspect");
    if (aspect <= 0.0f) {
        return 0.0f;
    }
    float clamped = std::min(std::clamp(aspect, kDefaultAspect, 32.0f / 9.0f), screen_aspect());
    return (screen_height() * clamped - screen_width()) / 2.0f;
}

void flush_fonts(GuestState& s) { guest_call<Conv::cdecl_>(s, kDrawFonts); }

class DrawScope {
public:
    DrawScope(GuestState& s, float offset) : s_(s), previous_(d3d9::offset_2d_x()) {
        flush_fonts(s_);
        d3d9::set_offset_2d_x(offset);
    }
    ~DrawScope() {
        flush_fonts(s_);
        d3d9::set_offset_2d_x(previous_);
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;

private:
    GuestState& s_;
    float previous_;
};

void return_to_caller(GuestState& s) {
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
}

Layout layout_of(uint32_t script) {
    char name[9] = {};
    std::memcpy(name, mem::ptr(script + 8), 8);
    std::string lower;
    for (char c : std::string_view(name)) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (auto n : {"pool2", "lowr", "wof", "sweet6"}) {
        if (lower == n) {
            return Layout::left_panels;
        }
    }
    for (auto n : {"toreno1", "tria", "mtbiker", "cprace"}) {
        if (lower == n) {
            return Layout::right_panels;
        }
    }
    for (auto n : {"blackj", "roulete"}) {
        if (lower == n) {
            return Layout::window_panels;
        }
    }
    return lower == "otb" ? Layout::table : Layout::center;
}

void attribute_new_records() {
    size_t texts = std::min<size_t>(mem::read<uint16_t>(kScriptTextsUsed), kScriptTextCount);
    size_t rects = std::min<size_t>(mem::read<uint16_t>(kScriptRectsUsed), kScriptRectCount);
    if (texts < g_text_mark) {
        g_text_mark = 0;
    }
    if (rects < g_rect_mark) {
        g_rect_mark = 0;
    }
    if (g_last_script) {
        Layout layout = layout_of(g_last_script);
        for (size_t i = g_text_mark; i < texts; ++i) {
            g_text_layouts[i] = layout;
            g_text_owners[i] = g_last_script;
        }
        for (size_t i = g_rect_mark; i < rects; ++i) {
            g_rect_layouts[i] = layout;
            g_rect_owners[i] = g_last_script;
        }
    }
    g_text_mark = texts;
    g_rect_mark = rects;
}

uint32_t script_command(GuestState& s, uint32_t pc) {
    attribute_new_records();
    g_last_script = ecx(s);
    return pc;
}

uint32_t text_addr(size_t i) { return kScriptTexts + static_cast<uint32_t>(i) * kScriptTextSize; }

uint32_t rect_addr(size_t i) { return kScriptRects + static_cast<uint32_t>(i) * kScriptRectSize; }

float help_text_scale() { return mem::read<float>(mem::read<uint32_t>(kHelpTextScaleOperand)); }

float script_left_offset() {
    float help_box_margin = screen_width() * help_text_scale() * 34.0f - 5.0f;
    return -hud_offset() + help_box_margin - scale_x(29.0f);
}

float script_offset(Layout layout, float left, float right) {
    if (layout == Layout::left_panels && right < 240.0f) {
        return script_left_offset();
    }
    if (layout == Layout::right_panels && left > 400.0f) {
        return -2.0f * offset_43() + hud_offset();
    }
    return -offset_43();
}

float rect_offset(size_t i, float sx) {
    uint32_t r = rect_addr(i);
    int32_t type = mem::read<int32_t>(r + rect::type);
    float ax = mem::read<float>(r + rect::ax);
    float right = type == 1 ? ax : mem::read<float>(r + rect::bx);
    float left_x = std::min(ax, right) / sx;
    float right_x = std::max(ax, right) / sx;
    Layout layout = g_rect_layouts[i];
    if (layout == Layout::window_panels && type == 2) {
        if (right_x < 240.0f) {
            layout = Layout::left_panels;
        } else if (left_x > 400.0f) {
            layout = Layout::right_panels;
        }
    }
    return script_offset(layout, left_x, right_x);
}

float text_offset(size_t i, float sx) {
    uint32_t t = text_addr(i);
    float tx = mem::read<float>(t + text::x);
    if (g_text_layouts[i] != Layout::center) {
        float x = tx * sx;
        float y = mem::read<float>(t + text::y) * screen_height() / 448.0f;
        uint8_t fade = mem::read<uint8_t>(t + text::before_fade);
        for (size_t j = 0; j < kScriptRectCount; ++j) {
            uint32_t r = rect_addr(j);
            if (mem::read<int32_t>(r + rect::type) != 2 || mem::read<uint8_t>(r + rect::before_fade) != fade ||
                g_rect_owners[j] != g_text_owners[i]) {
                continue;
            }
            float ax = mem::read<float>(r + rect::ax);
            float bx = mem::read<float>(r + rect::bx);
            float ay = mem::read<float>(r + rect::ay);
            float by = mem::read<float>(r + rect::by);
            if (x >= std::min(ax, bx) && x <= std::max(ax, bx) && y >= std::min(ay, by) && y <= std::max(ay, by)) {
                return rect_offset(j, sx);
            }
        }
    }
    return script_offset(g_text_layouts[i], tx, tx);
}

float script_text_width(GuestState& s, size_t index) {
    static const uint32_t key_buffer = allocator().alloc(16);
    static const uint32_t formatted = allocator().alloc(400);
    uint32_t t = text_addr(index);
    std::memset(mem::ptr(key_buffer), 0, 16);
    std::memcpy(mem::ptr(key_buffer), mem::ptr(t + text::key), 8);
    uint32_t string = guest_call<Conv::thiscall, uint32_t>(s, kGetTextString, kTheText, key_buffer);
    guest_call<Conv::cdecl_>(s, kInsertNumbers, string, mem::read<uint32_t>(t + text::numbers),
                             mem::read<uint32_t>(t + text::numbers + 4), 0xffffffffu, 0xffffffffu, 0xffffffffu,
                             0xffffffffu, formatted);
    uint16_t used = mem::read<uint16_t>(kScriptTextsUsed);
    mem::write<uint16_t>(kScriptTextsUsed, static_cast<uint16_t>(index));
    float width = guest_call<Conv::cdecl_, float>(s, kGetStringWidth, formatted, 1u, 1u);
    mem::write<uint16_t>(kScriptTextsUsed, used);
    return width;
}

void fit_script_text(GuestState& s, size_t index) {
    uint32_t t = text_addr(index);
    if (mem::read<uint8_t>(t + text::centered) || mem::read<uint8_t>(t + text::right_justify) ||
        mem::read<uint8_t>(t + text::justify)) {
        return;
    }
    char raw[9] = {};
    std::memcpy(raw, mem::ptr(t + text::key), 8);
    std::string_view key(raw);
    float x = mem::read<float>(t + text::x);
    float available = 0.0f;
    if (g_text_layouts[index] == Layout::left_panels && (key == "PL_05" || key == "PL_06")) {
        available = mem::read<float>(t + text::wrap_x) - x - 0.5f;
    } else if (g_text_layouts[index] == Layout::table) {
        float wrap = mem::read<float>(t + text::wrap_x);
        float right = wrap;
        uint8_t fade = mem::read<uint8_t>(t + text::before_fade);
        float y = mem::read<float>(t + text::y);
        for (size_t i = 0; i < kScriptTextCount; ++i) {
            uint32_t c = text_addr(i);
            if (!mem::read<uint8_t>(c + text::key) || !mem::read<uint8_t>(c + text::right_justify) ||
                mem::read<uint8_t>(c + text::before_fade) != fade || g_text_owners[i] != g_text_owners[index] ||
                mem::read<float>(c + text::y) != y || mem::read<float>(c + text::x) <= x) {
                continue;
            }
            right = std::min(right, mem::read<float>(c + text::x) - script_text_width(s, i) - 8.0f);
        }
        if (right < wrap) {
            available = right - x;
        }
    }
    if (available > 0.0f) {
        float width = script_text_width(s, index);
        if (width > available) {
            mem::write<float>(t + text::scale_x, mem::read<float>(t + text::scale_x) * available / width);
        }
    }
}

Memory* draw_script_text(GuestState& s, uint32_t, Memory* m) {
    uint8_t priority = mem::read<uint8_t>(esp(s) + 4);
    attribute_new_records();
    flush_fonts(s);
    std::vector<uint8_t> texts(mem::ptr(kScriptTexts), mem::ptr(kScriptTexts) + kScriptTextCount * kScriptTextSize);
    std::vector<uint8_t> rects(mem::ptr(kScriptRects), mem::ptr(kScriptRects) + kScriptRectCount * kScriptRectSize);
    float sx = scale_x(1.0f);
    for (size_t i = 0; i < kScriptTextCount; ++i) {
        uint32_t t = text_addr(i);
        if (!mem::read<uint8_t>(t + text::key) || mem::read<uint8_t>(t + text::before_fade) != priority) {
            continue;
        }
        fit_script_text(s, i);
        float offset = text_offset(i, sx) / sx;
        mem::write<float>(t + text::x, mem::read<float>(t + text::x) + offset);
        if (!mem::read<uint8_t>(t + text::centered) && !mem::read<uint8_t>(t + text::right_justify)) {
            mem::write<float>(t + text::wrap_x, mem::read<float>(t + text::wrap_x) + offset);
        }
    }
    for (size_t i = 0; i < kScriptRectCount; ++i) {
        uint32_t r = rect_addr(i);
        int32_t type = mem::read<int32_t>(r + rect::type);
        if (!type || mem::read<uint8_t>(r + rect::before_fade) != priority) {
            continue;
        }
        float offset = rect_offset(i, sx);
        mem::write<float>(r + rect::ax, mem::read<float>(r + rect::ax) + offset);
        if (type != 1) {
            mem::write<float>(r + rect::bx, mem::read<float>(r + rect::bx) + offset);
        }
    }
    guest_call<Conv::cdecl_>(s, redirect_original(), static_cast<uint32_t>(priority));
    flush_fonts(s);
    std::memcpy(mem::ptr(kScriptTexts), texts.data(), texts.size());
    std::memcpy(mem::ptr(kScriptRects), rects.data(), rects.size());
    return_to_caller(s);
    return m;
}

void display_script_menu(GuestState& s, uint32_t target) {
    uint32_t id = mem::read<uint8_t>(esp(s) + 4);
    uint32_t bright = mem::read<uint8_t>(esp(s) + 8);
    uint32_t panel = id < 2 ? mem::read<uint32_t>(kScriptMenus + 4 * id) : 0;
    bool drawing_map = mem::read<uint8_t>(kMenuManager + kDrawingMap) != 0;
    float offset = -offset_43();
    if (panel && !drawing_map) {
        float x = mem::read<float>(panel + kScriptMenuX) / scale_x(1.0f);
        if (x < 160.0f) {
            offset = script_left_offset();
        } else if (x > 480.0f) {
            offset = -2.0f * offset_43() + hud_offset();
        }
    } else if (drawing_map) {
        offset = 0.0f;
    }
    flush_fonts(s);
    float original = panel ? mem::read<float>(panel + kScriptMenuX) : 0.0f;
    if (panel) {
        mem::write<float>(panel + kScriptMenuX, original + offset);
    }
    guest_call<Conv::cdecl_>(s, target, id, bright);
    flush_fonts(s);
    if (panel) {
        mem::write<float>(panel + kScriptMenuX, original);
    }
}

Memory* script_menu_call(GuestState& s, uint32_t, Memory* m) {
    display_script_menu(s, redirect_original());
    return_to_caller(s);
    return m;
}

uint32_t script_menu_tail(GuestState& s, uint32_t target) {
    display_script_menu(s, target);
    return_to_caller(s);
    return eip(s);
}

uint32_t script_menu_tail_a(GuestState& s, uint32_t) { return script_menu_tail(s, kScriptMenuA); }

uint32_t script_menu_tail_b(GuestState& s, uint32_t) { return script_menu_tail(s, kScriptMenuB); }

Memory* menu_page(GuestState& s, uint32_t, Memory* m) {
    uint32_t menu = ecx(s);
    {
        DrawScope scope(s, -offset_43());
        guest_call<Conv::thiscall>(s, redirect_original(), menu);
    }
    return_to_caller(s);
    return m;
}

Memory* hud_element(GuestState& s, float offset, Memory* m) {
    if (offset == 0.0f) {
        guest_call<Conv::cdecl_>(s, redirect_original());
    } else {
        DrawScope scope(s, offset);
        guest_call<Conv::cdecl_>(s, redirect_original());
    }
    return_to_caller(s);
    return m;
}

Memory* hud_element_left(GuestState& s, uint32_t, Memory* m) { return hud_element(s, -hud_offset(), m); }

Memory* hud_element_right(GuestState& s, uint32_t, Memory* m) { return hud_element(s, hud_offset(), m); }

struct Canvas {
    int depth = 0;
    int suspensions = 0;
    int32_t physical_width = 0;
    float offset = 0.0f;
};

Canvas g_canvas;

int32_t canvas_width(int32_t width, int32_t height) {
    static const float aspect = aspect_setting("widescreen.menu_aspect");
    if (aspect <= 0.0f || height <= 0) {
        return width;
    }
    float clamped = std::min(std::clamp(aspect, kDefaultAspect, 32.0f / 9.0f), static_cast<float>(width) / height);
    int32_t minimum = std::min(width, static_cast<int32_t>(std::lround(height * kDefaultAspect)));
    auto canvas = static_cast<int32_t>(
        std::lround(std::clamp(height * clamped, static_cast<float>(minimum), static_cast<float>(width))));
    if ((width - canvas) & 1) {
        ++canvas;
    }
    return canvas;
}

void canvas_apply() {
    g_canvas.physical_width = mem::read<int32_t>(kRsGlobalWidth);
    int32_t width = canvas_width(g_canvas.physical_width, mem::read<int32_t>(kRsGlobalHeight));
    g_canvas.offset = static_cast<float>(g_canvas.physical_width - width) * 0.5f;
    mem::write<int32_t>(kRsGlobalWidth, width);
    refresh_operand_slots();
}

void canvas_restore() {
    mem::write<int32_t>(kRsGlobalWidth, g_canvas.physical_width);
    refresh_operand_slots();
}

void flush_menu_fonts(GuestState& s) {
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    if (engine && mem::read<uint32_t>(engine)) {
        flush_fonts(s);
    }
}

bool fullscreen_map() {
    static const bool on = mod_settings().flag("widescreen.fullscreen_map");
    return on;
}

void menu_with_canvas(GuestState& s, uint32_t target, uint32_t menu) {
    bool active = mem::read<uint8_t>(menu + kMenuActive) != 0 &&
                  !(fullscreen_map() && mem::read<uint8_t>(menu + kCurrentScreen) == kScreenMap) &&
                  canvas_width(mem::read<int32_t>(kRsGlobalWidth), mem::read<int32_t>(kRsGlobalHeight)) <
                      mem::read<int32_t>(kRsGlobalWidth);
    if (!active || g_canvas.depth) {
        guest_call<Conv::thiscall>(s, target, menu);
        return;
    }
    flush_menu_fonts(s);
    canvas_apply();
    ++g_canvas.depth;
    float previous = d3d9::offset_2d_x();
    d3d9::set_offset_2d_x(g_canvas.offset);
    d3d9::set_mask_widening(g_canvas.offset, static_cast<float>(mem::read<int32_t>(kRsGlobalWidth)), screen_height());
    auto inset = static_cast<int32_t>(g_canvas.offset);
    for (uint32_t f : kMouseFields) {
        mem::write<int32_t>(menu + f, mem::read<int32_t>(menu + f) - inset);
    }
    guest_call<Conv::thiscall>(s, target, menu);
    flush_menu_fonts(s);
    for (uint32_t f : kMouseFields) {
        mem::write<int32_t>(menu + f, mem::read<int32_t>(menu + f) + inset);
    }
    d3d9::set_offset_2d_x(previous);
    d3d9::set_mask_widening(0.0f, 0.0f, 0.0f);
    --g_canvas.depth;
    canvas_restore();
}

Memory* menu_draw_call(GuestState& s, uint32_t, Memory* m) {
    menu_with_canvas(s, redirect_original(), ecx(s));
    return_to_caller(s);
    return m;
}

uint32_t menu_draw_tail(GuestState& s, uint32_t) {
    menu_with_canvas(s, kMenuDraw, ecx(s));
    return_to_caller(s);
    return eip(s);
}

Memory* menu_input_call(GuestState& s, uint32_t, Memory* m) {
    menu_with_canvas(s, redirect_original(), ecx(s));
    return_to_caller(s);
    return m;
}

uint32_t menu_mouse_range(GuestState& s, uint32_t pc) {
    if (g_canvas.depth && !g_canvas.suspensions) {
        auto inset = static_cast<int32_t>(g_canvas.offset);
        int32_t raw = mem::read<int32_t>(esi(s) + kMouseRawX);
        mem::write<int32_t>(esi(s) + kMouseFields[0], std::clamp(raw, -inset, g_canvas.physical_width - inset));
    }
    return pc;
}

template <bool HasMode>
Memory* physical_screen_call(GuestState& s, uint32_t, Memory* m) {
    bool suspend = g_canvas.depth && !g_canvas.suspensions++;
    if (suspend) {
        canvas_restore();
        d3d9::set_offset_2d_x(0.0f);
        d3d9::set_mask_widening(0.0f, 0.0f, 0.0f);
    }
    if constexpr (HasMode) {
        guest_call<Conv::cdecl_>(s, redirect_original(), mem::read<uint32_t>(esp(s) + 4));
    } else {
        guest_call<Conv::cdecl_>(s, redirect_original());
    }
    if (g_canvas.depth && !--g_canvas.suspensions) {
        canvas_apply();
        d3d9::set_offset_2d_x(g_canvas.offset);
        d3d9::set_mask_widening(g_canvas.offset, static_cast<float>(mem::read<int32_t>(kRsGlobalWidth)),
                                screen_height());
    }
    return_to_caller(s);
    return m;
}

struct Rect {
    float left, bottom, right, top;
};

Rect read_rect(uint32_t addr) {
    return {mem::read<float>(addr), mem::read<float>(addr + 4), mem::read<float>(addr + 8), mem::read<float>(addr + 12)};
}

void draw_black(GuestState& s, Rect r, uint8_t alpha) {
    static const uint32_t rect = allocator().alloc(16);
    static const uint32_t color = allocator().alloc(4);
    mem::write<float>(rect, r.left);
    mem::write<float>(rect + 4, r.bottom);
    mem::write<float>(rect + 8, r.right);
    mem::write<float>(rect + 12, r.top);
    mem::write<uint32_t>(color, static_cast<uint32_t>(alpha) << 24);
    guest_call<Conv::cdecl_>(s, kDrawRect, rect, color);
}

std::string texture_name(uint32_t texture) {
    char name[33] = {};
    std::memcpy(name, mem::ptr(texture + kTextureName), 32);
    return name;
}

std::pair<int32_t, int32_t> raster_size(uint32_t texture) {
    uint32_t raster = mem::read<uint32_t>(texture);
    if (!raster) {
        return {0, 0};
    }
    return {mem::read<int32_t>(raster + kRasterWidth), mem::read<int32_t>(raster + kRasterHeight)};
}

Rect content_rect(uint32_t texture, Rect rect) {
    float aspect = kDefaultAspect;
    auto [w, h] = raster_size(texture);
    if (w > 0 && h > 0 && w != h && static_cast<int64_t>(w) != 2LL * h) {
        aspect = static_cast<float>(w) / static_cast<float>(h);
    }
    float center = screen_width() / 2.0f;
    float half = screen_height() * aspect / 2.0f;
    return {center - half, rect.bottom, center + half, rect.top};
}

bool is_fullscreen(Rect r) {
    if (mem::read<uint8_t>(kMenuManager + kDrawingMap)) {
        return false;
    }
    return r.left <= 0.5f && r.top <= 0.5f && r.right >= screen_width() - 0.5f && r.bottom >= screen_height() - 0.5f;
}

uint32_t sprite_draw(GuestState& s, uint32_t pc) {
    static const uint32_t replacement = allocator().alloc(16);
    uint32_t sprite = ecx(s);
    uint32_t texture = mem::read<uint32_t>(sprite);
    uint32_t rect_addr = mem::read<uint32_t>(esp(s) + 4);
    uint8_t alpha = mem::read<uint8_t>(mem::read<uint32_t>(esp(s) + 8) + 3);
    uint32_t saved_eax = eax(s);
    uint32_t saved_edx = edx(s);
    if (texture) {
        std::string name = texture_name(texture);
        float pillar = -offset_43();
        if ((name == "tvcorn" || name == "backbet") && pillar > 0.0f) {
            draw_black(s, {0.0f, 0.0f, pillar, screen_height()}, 255);
            draw_black(s, {screen_width() - pillar, 0.0f, screen_width(), screen_height()}, 255);
        }
    }
    Rect rect = read_rect(rect_addr);
    if (texture && is_fullscreen(rect)) {
        Rect content = content_rect(texture, rect);
        content.top = 0.0f;
        content.bottom = screen_height();
        mem::write<float>(replacement, content.left);
        mem::write<float>(replacement + 4, content.bottom);
        mem::write<float>(replacement + 8, content.right);
        mem::write<float>(replacement + 12, content.top);
        mem::write<uint32_t>(esp(s) + 4, replacement);
        draw_black(s, {0.0f, rect.top, content.left, rect.bottom}, alpha);
        draw_black(s, {content.right, rect.top, screen_width(), rect.bottom}, alpha);
    }
    ecx(s) = sprite;
    eax(s) = saved_eax;
    edx(s) = saved_edx;
    return pc;
}

bool loading_artwork(const std::string& name) {
    if (!name.starts_with("loadsc")) {
        return false;
    }
    return (name.size() == 7 && name[6] >= '1' && name[6] <= '9') ||
           (name.size() == 8 && name[6] == '1' && name[7] >= '0' && name[7] <= '4');
}

uint32_t loading_bar(GuestState& s, uint32_t pc) {
    int32_t index = mem::read<int32_t>(kCurrentSplash);
    if (index < 0 || index >= static_cast<int32_t>(kSplashCount)) {
        return pc;
    }
    uint32_t texture = mem::read<uint32_t>(kSplashSprites + 4 * static_cast<uint32_t>(index));
    if (!texture) {
        return pc;
    }
    float w = screen_width();
    float h = screen_height();
    Rect content = content_rect(texture, {0.0f, h, w, 0.0f});
    float content_width = content.right - content.left;
    auto [rw, rh] = raster_size(texture);
    Rect bar;
    if (loading_artwork(texture_name(texture)) && rw > 0 && rh > 0 && int64_t(rw) * 9 == int64_t(rh) * 16) {
        bar = {std::round(content.left + content_width * (111.0f / 1920.0f)), std::round(h * (1008.0f / 1080.0f)),
               std::round(content.left + content_width * (521.0f / 1920.0f)), std::round(h * (984.0f / 1080.0f))};
    } else {
        float x = content.left + content_width * (50.0f / 640.0f);
        float y = h * (408.0f / 448.0f);
        bar = {x, y + h * (10.0f / 448.0f), x + content_width * (180.0f / 640.0f), y};
    }
    mem::write<float>(esp(s), bar.left);
    mem::write<float>(esp(s) + 4, bar.top);
    mem::write<uint32_t>(esp(s) + 8, static_cast<uint32_t>(std::clamp(bar.right - bar.left, 1.0f, 65535.0f)));
    mem::write<uint32_t>(esp(s) + 12, static_cast<uint32_t>(std::clamp(bar.bottom - bar.top, 1.0f, 255.0f)));
    return pc;
}

float slider_left(float) { return screen_width() - scale_x(140.0f); }

float slider_right(float) { return screen_width(); }

Memory* display_slider(GuestState& s, uint32_t, Memory* m) {
    std::array<uint32_t, 7> args{};
    for (uint32_t i = 0; i < 7; ++i) {
        args[i] = mem::read<uint32_t>(esp(s) + 4 + 4 * i);
    }
    float x = screen_width() - scale_x(140.0f);
    float width = scale_x(100.0f);
    auto notch = static_cast<int32_t>(std::lround(scale_x(3.0f)));
    std::memcpy(&args[0], &x, 4);
    std::memcpy(&args[4], &width, 4);
    args[6] = static_cast<uint32_t>(notch);
    auto result = guest_call<Conv::stdcall, int32_t>(s, redirect_original(), args[0], args[1], args[2], args[3],
                                                      args[4], args[5], args[6]);
    eax(s) = static_cast<uint32_t>(std::max(static_cast<int32_t>(x), result));
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4 + 28;
    return m;
}

struct MapCanvas {
    float center, inset, right, bottom;
};

MapCanvas map_canvas() {
    float aspect;
    if (fullscreen_map()) {
        aspect = mem::read<float>(kDrawAspectRatio);
    } else {
        int32_t physical = g_canvas.depth ? g_canvas.physical_width : mem::read<int32_t>(kRsGlobalWidth);
        int32_t height = mem::read<int32_t>(kRsGlobalHeight);
        aspect = height > 0 ? static_cast<float>(canvas_width(physical, height)) / static_cast<float>(height)
                            : kDefaultAspect;
    }
    float width = 480.0f * aspect;
    float inset = fullscreen_map() ? 0.0f : 60.0f;
    return {width * 0.5f, inset, width - inset, 448.0f - inset};
}

float map_center(float) {
    static float last = 320.0f;
    float center = map_canvas().center;
    if (center != last) {
        if (mem::read<uint8_t>(kMenuManager + kMapLoaded)) {
            uint32_t origin = kMenuManager + kMapOriginX;
            mem::write<float>(origin, mem::read<float>(origin) + center - last);
        }
        last = center;
    }
    return center;
}

float map_inset(float) { return map_canvas().inset; }

float map_right(float) { return map_canvas().right; }

float map_bottom(float) { return map_canvas().bottom; }

float map_cursor_left(float) { return map_canvas().inset + 4.0f; }

float map_cursor_right(float) { return map_canvas().right - 4.0f; }

float map_cursor_bottom(float) { return map_canvas().bottom - 4.0f; }

float map_left_scale(float) { return map_canvas().inset / (map_canvas().center * 2.0f); }

float map_right_scale(float) { return map_canvas().right / (map_canvas().center * 2.0f); }

float map_top_scale(float) { return map_canvas().inset / 448.0f; }

float map_bottom_scale(float) { return map_canvas().bottom / 448.0f; }

float map_speed(float) { return 14.0f; }

float stdcall_map_left(float) { return scale_x(map_canvas().inset); }

float stdcall_map_right(float) { return scale_x(map_canvas().right); }

float stdcall_map_top(float) { return screen_height() * map_canvas().inset / 448.0f; }

float stdcall_map_bottom(float) { return screen_height() * map_canvas().bottom / 448.0f; }

float stdcall_map_center(float) { return scale_x(map_canvas().center); }

void clamp_map_origin(uint32_t menu) {
    float zoom = mem::read<float>(menu + kMapZoom);
    auto clamp = [zoom](float center, float left, float right) {
        float midpoint = (left + right) * 0.5f;
        float range = std::max(zoom - (right - left) * 0.5f, 0.0f);
        return std::clamp(center, midpoint - range, midpoint + range);
    };
    MapCanvas c = map_canvas();
    mem::write<float>(menu + kMapOriginX, clamp(mem::read<float>(menu + kMapOriginX), c.inset, c.right));
    mem::write<float>(menu + kMapOriginY, clamp(mem::read<float>(menu + kMapOriginY), c.inset, c.bottom));
}

uint32_t map_init_origin(GuestState& s, uint32_t) {
    mem::write<float>(edi(s) + kMapOriginX, map_canvas().center);
    return kMapInitResume;
}

uint32_t map_clamp_origin(GuestState& s, uint32_t) {
    clamp_map_origin(esi(s));
    return kMapClampResume;
}

uint32_t map_clamp_streaming(GuestState& s, uint32_t pc) {
    clamp_map_origin(ebx(s));
    return pc;
}

uint32_t map_clamp_draw(GuestState& s, uint32_t pc) {
    clamp_map_origin(edi(s));
    return pc;
}

uint32_t map_zoom_reference(GuestState& s, uint32_t pc) {
    static const uint32_t point = allocator().alloc(8);
    uint32_t menu = esi(s);
    auto saved = std::make_unique<GuestState>(s);
    guest_call<Conv::cdecl_>(s, kMapTransformPoint, point, menu + kMapMouse);
    s = *saved;
    float x = mem::read<float>(point);
    float y = mem::read<float>(point + 4);
    float zoom = mem::read<float>(menu + kMapZoom);
    mem::write<float>(esp(s) + 0x2c, x * zoom);
    mem::write<float>(esp(s) + 0x7c, -y * zoom);
    mem::write<float>(esp(s) + 0x84, x);
    mem::write<float>(esp(s) + 0x10, -y);
    return pc;
}

bool g_drawing_map_overview = false;

float overview_x(float x, float center) { return center + (x - center) * (480.0f / 448.0f); }

float map_origin_x() { return mem::read<float>(kMenuManager + kMapOriginX); }

uint32_t map_overview_begin(GuestState&, uint32_t pc) {
    g_drawing_map_overview = true;
    return pc;
}

uint32_t map_overview_end(GuestState&, uint32_t pc) {
    g_drawing_map_overview = false;
    return pc;
}

uint32_t map_overview_rect(GuestState& s, uint32_t pc) {
    if (g_drawing_map_overview) {
        uint32_t rect = mem::read<uint32_t>(esp(s));
        float center = scale_x(map_origin_x());
        mem::write<float>(rect, overview_x(mem::read<float>(rect), center));
        mem::write<float>(rect + 8, overview_x(mem::read<float>(rect + 8), center));
    }
    return pc;
}

uint32_t map_overview_projection(GuestState& s, uint32_t pc) {
    if (g_drawing_map_overview) {
        mem::write<float>(ecx(s), overview_x(mem::read<float>(ecx(s)), map_origin_x()));
    }
    return pc;
}

uint32_t map_overview_limit_begin(GuestState& s, uint32_t pc) {
    if (g_drawing_map_overview) {
        float center = scale_x(map_origin_x());
        mem::write<float>(ecx(s), center + (mem::read<float>(ecx(s)) - center) * (448.0f / 480.0f));
    }
    return pc;
}

uint32_t map_overview_limit_end(GuestState& s, uint32_t pc) {
    if (g_drawing_map_overview) {
        mem::write<float>(ecx(s), overview_x(mem::read<float>(ecx(s)), scale_x(map_origin_x())));
    }
    return pc;
}

uint32_t map_area_name(GuestState& s, uint32_t pc) {
    if (fullscreen_map()) {
        uint32_t y = esp(s) + 4;
        mem::write<float>(y, mem::read<float>(y) - screen_height() * (40.0f / 448.0f));
    }
    return pc;
}

float legend_offset() { return (screen_width() - scale_x(640.0f)) * 0.5f; }

uint32_t map_legend_window(GuestState& s, uint32_t pc) {
    uint32_t rect = mem::read<uint32_t>(esp(s));
    float offset = legend_offset();
    mem::write<float>(rect, mem::read<float>(rect) + offset);
    mem::write<float>(rect + 8, mem::read<float>(rect + 8) + offset);
    return pc;
}

uint32_t map_legend_entry(GuestState& s, uint32_t pc) {
    mem::write<int32_t>(esp(s), mem::read<int32_t>(esp(s)) + static_cast<int32_t>(std::lround(legend_offset())));
    return pc;
}

Memory* skip_call(GuestState& s, uint32_t, Memory* m) {
    return_to_caller(s);
    return m;
}

uint32_t& register32(GuestState& s, uint32_t index) {
    switch (index & 7) {
    case 0: return eax(s);
    case 1: return ecx(s);
    case 2: return edx(s);
    case 3: return ebx(s);
    case 4: return esp(s);
    case 5: return ebp(s);
    case 6: return esi(s);
    default: return edi(s);
    }
}

void set_compare_flags(GuestState& s, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    s.aflag.cf = a < b;
    s.aflag.zf = r == 0;
    s.aflag.sf = static_cast<uint8_t>(r >> 31);
    s.aflag.of = static_cast<uint8_t>(((a ^ b) & (a ^ r)) >> 31);
    s.aflag.pf = (std::popcount(r & 0xffu) & 1) == 0;
    s.aflag.af = static_cast<uint8_t>(((a ^ b ^ r) >> 4) & 1);
}

constexpr uint32_t kNoWidth = 0xffffffff;

uint32_t width_640(GuestState& s, uint32_t pc) {
    uint8_t op = mem::read<uint8_t>(pc);
    if (op == 0x3d) {
        set_compare_flags(s, eax(s), kNoWidth);
        return pc + 5;
    }
    if (op == 0xbe) {
        esi(s) = kNoWidth;
        return pc + 5;
    }
    uint8_t modrm = mem::read<uint8_t>(pc + 1);
    if (op == 0x81 && modrm == 0x3d) {
        set_compare_flags(s, mem::read<uint32_t>(mem::read<uint32_t>(pc + 2)), kNoWidth);
        return pc + 10;
    }
    if (op == 0x81 && (modrm & 0xf8) == 0xf8) {
        set_compare_flags(s, register32(s, modrm), kNoWidth);
        return pc + 6;
    }
    return pc;
}

float stretch_x(float x) {
    return static_cast<float>(screen_width() * static_cast<double>(x) * mem::read<float>(kResXInv));
}

Memory* radio_scan_rect(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t rect = allocator().alloc(16);
    uint32_t original = mem::read<uint32_t>(esp(s) + 4);
    uint32_t color = mem::read<uint32_t>(esp(s) + 8);
    float offset = screen_width() * 0.5f - scale_x(320.0f);
    for (uint32_t i = 0; i < 16; i += 4) {
        float v = mem::read<float>(original + i);
        mem::write<float>(rect + i, (i == 0 || i == 8) ? v + offset : v);
    }
    guest_call<Conv::cdecl_>(s, redirect_original(), rect, color);
    return_to_caller(s);
    return m;
}

template <int Percent>
Memory* subtitle_centre_size(GuestState& s, uint32_t, Memory* m) {
    float width = mem::read<float>(esp(s) + 4);
    width = std::max(1.0f, width + 2.0f * std::min(hud_offset(), 0.0f) * (static_cast<float>(Percent) / 100.0f));
    guest_call<Conv::cdecl_>(s, redirect_original(), std::bit_cast<uint32_t>(width));
    return_to_caller(s);
    return m;
}

uint32_t coop_crosshair(GuestState& s, uint32_t pc) {
    x87_set_st0(s, x87_st0(s) * mem::read<float>(esp(s) + 0x34));
    return pc + 4;
}

constexpr NativeSymbol kSymbols[] = {
    {"ws_stretch_x", native_trace<Conv::stdcall, stretch_x>},
    {"ws_radio_scan_rect", radio_scan_rect},
    {"ws_subtitle_centre_size", subtitle_centre_size<100>},
    {"ws_subtitle_centre_size_stats", subtitle_centre_size<80>},
    {"ws_skip_call", skip_call},
    {"ws_map_left_x", native_trace<Conv::stdcall, stdcall_map_left>},
    {"ws_map_right_x", native_trace<Conv::stdcall, stdcall_map_right>},
    {"ws_map_top_y", native_trace<Conv::stdcall, stdcall_map_top>},
    {"ws_map_bottom_y", native_trace<Conv::stdcall, stdcall_map_bottom>},
    {"ws_map_center_x", native_trace<Conv::stdcall, stdcall_map_center>},
    {"ws_display_slider", display_slider},
    {"ws_slider_left", native_trace<Conv::stdcall, slider_left>},
    {"ws_slider_right", native_trace<Conv::stdcall, slider_right>},
    {"ws_menu_draw_call", menu_draw_call},
    {"ws_menu_input_call", menu_input_call},
    {"ws_menu_video_mode", physical_screen_call<true>},
    {"ws_menu_centre_mouse", physical_screen_call<false>},
    {"ws_draw_script_text", draw_script_text},
    {"ws_script_menu_call", script_menu_call},
    {"ws_menu_page", menu_page},
    {"ws_hud_element_left", hud_element_left},
    {"ws_hud_element_right", hud_element_right},
};

constexpr MidHookSymbol kMidHooks[] = {
    {"ws_width_640", width_640},
    {"ws_coop_crosshair", coop_crosshair},
    {"ws_script_command", script_command},
    {"ws_script_menu_tail_a", script_menu_tail_a},
    {"ws_script_menu_tail_b", script_menu_tail_b},
    {"ws_menu_draw_tail", menu_draw_tail},
    {"ws_menu_mouse_range", menu_mouse_range},
    {"ws_sprite_draw", sprite_draw},
    {"ws_map_init_origin", map_init_origin},
    {"ws_map_clamp_origin", map_clamp_origin},
    {"ws_map_clamp_streaming", map_clamp_streaming},
    {"ws_map_clamp_draw", map_clamp_draw},
    {"ws_map_zoom_reference", map_zoom_reference},
    {"ws_map_overview_begin", map_overview_begin},
    {"ws_map_overview_end", map_overview_end},
    {"ws_map_overview_rect", map_overview_rect},
    {"ws_map_overview_projection", map_overview_projection},
    {"ws_map_overview_limit_begin", map_overview_limit_begin},
    {"ws_map_overview_limit_end", map_overview_limit_end},
    {"ws_map_area_name", map_area_name},
    {"ws_map_legend_window", map_legend_window},
    {"ws_map_legend_entry", map_legend_entry},
    {"ws_loading_bar", loading_bar},
};

constexpr SlotSymbol kSlots[] = {
    {"ws_map_center", map_center},
    {"ws_map_inset", map_inset},
    {"ws_map_right", map_right},
    {"ws_map_bottom", map_bottom},
    {"ws_map_cursor_left", map_cursor_left},
    {"ws_map_cursor_right", map_cursor_right},
    {"ws_map_cursor_bottom", map_cursor_bottom},
    {"ws_map_left_scale", map_left_scale},
    {"ws_map_right_scale", map_right_scale},
    {"ws_map_top_scale", map_top_scale},
    {"ws_map_bottom_scale", map_bottom_scale},
    {"ws_map_speed", map_speed},
};

}

std::span<const SlotSymbol> widescreen_hud_slots() { return kSlots; }

std::span<const NativeSymbol> widescreen_hud_symbols() { return kSymbols; }

std::span<const MidHookSymbol> widescreen_hud_mid_hooks() { return kMidHooks; }

}
