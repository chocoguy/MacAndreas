#include "natives.hpp"

#include "../guest_memory.hpp"
#include "../mod_settings.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace ma::host::native {

namespace {

constexpr uint32_t kRsGlobalWidth = 0x00c17044;
constexpr uint32_t kRsGlobalHeight = 0x00c17048;
constexpr uint32_t kFov = 0x008d5038;
constexpr uint32_t kAspectRatio = 0x00c3efa4;
constexpr uint32_t kCutsceneRunning = 0x00b5f851;
constexpr uint32_t kCameraWideScreenOn = 0x00b6f065;
constexpr uint32_t kFrustumResume = 0x00514d70;
constexpr uint32_t kAimWidthResumeA = 0x0050ad5f;
constexpr uint32_t kAimWidthResumeB = 0x00514993;
constexpr uint32_t kAimDivideResume = 0x0050ad7f;
constexpr uint32_t kSkySpreadResumeA = 0x00714847;
constexpr uint32_t kSkySpreadResumeB = 0x00714864;
constexpr float kSkySpread = 1.4f;
constexpr uint32_t kSpriteWidthResumeA = 0x0070cede;
constexpr uint32_t kSpriteWidthResumeB = 0x0071da73;
constexpr float kDefaultAspect = 4.0f / 3.0f;
constexpr float kCameraWidth = 0.01403292f;
constexpr double kMaxViewWidth = 3.7320508;
constexpr uint32_t kTimeInMilliseconds = 0x00b7cb84;
constexpr uint32_t kDrawRect = 0x00727b60;
constexpr uint32_t kCameraWideScreenOnField = 0x3d;
constexpr uint32_t kCameraFading = 0x51;
constexpr uint32_t kCameraBlurType = 0x90;
constexpr uint32_t kCameraMotionBlurAlpha = 0xa4;
constexpr uint32_t kCameraFadeAlpha = 0xbfc;
constexpr uint32_t kCameraFadeDuration = 0xc04;
constexpr uint32_t kBlurNone = 0;
constexpr uint32_t kBlurLightScene = 2;
constexpr uint32_t kBorderCallA = 0x0053e2af;
constexpr uint32_t kBorderCallB = 0x005af8bb;
constexpr uint32_t kHelpTextResume = 0x0058bb94;
constexpr uint32_t kCamFov = 0xb4;
constexpr uint32_t kWheelCamResume = 0x0051212c;
constexpr float kBorderAnimSeconds = 0.35f;
constexpr float kVanillaWindowFraction = 0.7f + 8.0f / 480.0f;
constexpr float kVanillaCenterFudgeY = 18.0f;
constexpr float kMinTopBarRatio = 0.2f;
constexpr uint32_t kFontScaleY = 0x00c71a68;
constexpr uint32_t kGetNumberLines = 0x0071a5e0;
constexpr float kHudWidth = 1.0f / 640.0f;
constexpr float kHudHeight = 1.0f / 448.0f;
constexpr float kWeaponIconWidth = 0.17343046f;
constexpr float kLockOnWidth = 20.0f;
constexpr float kWeaponIconLeft = 47.0f;
constexpr float kPlayerInfoBarsX = 94.0f;
constexpr float kSubtitlesWidth = 1.2f;
constexpr float kSubtitlesHeight = 0.58f;

float g_scaled_fov = 70.0f;

double radians(double deg) { return deg * std::numbers::pi / 180.0; }

double degrees(double rad) { return rad * 180.0 / std::numbers::pi; }

float screen_aspect() {
    int32_t width = mem::read<int32_t>(kRsGlobalWidth);
    int32_t height = mem::read<int32_t>(kRsGlobalHeight);
    return width > 0 && height > 0 ? static_cast<float>(width) / static_cast<float>(height) : kDefaultAspect;
}

float aspect_ratio() { return mem::read<float>(kAspectRatio); }

float hor_plus(float hfov) {
    double vfov = 2.0 * std::atan(std::tan(radians(hfov) / 2.0) / kDefaultAspect);
    return static_cast<float>(degrees(2.0 * std::atan(std::tan(vfov / 2.0) * aspect_ratio())));
}

float calculate_aspect_ratio() {
    static std::atomic<bool> logged = false;
    float ratio = screen_aspect();
    mem::write<float>(kAspectRatio, ratio);
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[WS] aspect ratio %.4f (%dx%d)\n", ratio, mem::read<int32_t>(kRsGlobalWidth),
                     mem::read<int32_t>(kRsGlobalHeight));
    }
    return ratio;
}

float fov_factor() {
    static const float factor = std::clamp(static_cast<float>(mod_settings().number("widescreen.fov_factor")), 0.5f, 2.0f);
    return factor;
}

float gameplay_fov(float fov) {
    double width = std::tan(radians(hor_plus(fov)) / 2.0);
    double scaled = width * fov_factor();
    if (scaled > kMaxViewWidth) {
        scaled = std::max(width, kMaxViewWidth);
    }
    return static_cast<float>(degrees(2.0 * std::atan(scaled)));
}

struct Borders {
    float mult = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    float pillar = 0.0f;
    float zoom = 1.0f;
    bool has_pillar = false;
    bool has_letterbox = false;
    bool was_fading = false;
    uint32_t last_tick = 0;
};

Borders g_borders;
float g_cutscene_zoom = 1.0f;
float g_sprite_scale = 1.0f;

enum class BorderMode { off, letterbox, pillarbox, both };

BorderMode border_mode() {
    static const BorderMode mode = [] {
        std::string v = mod_settings().choice("widescreen.cutscene_borders");
        return v == "off" ? BorderMode::off
               : v == "letterbox" ? BorderMode::letterbox
               : v == "pillarbox" ? BorderMode::pillarbox
                                  : BorderMode::both;
    }();
    return mode;
}

bool animated_borders() {
    static const bool animated = mod_settings().flag("widescreen.cutscene_border_animation");
    return animated;
}

void set_fov(float fov) {
    bool cutscene = mem::read<uint8_t>(kCutsceneRunning) || mem::read<uint8_t>(kCameraWideScreenOn);
    if (!cutscene) {
        g_cutscene_zoom = 1.0f;
        g_sprite_scale = 1.0f;
        g_scaled_fov = gameplay_fov(fov);
    } else {
        g_scaled_fov = g_cutscene_zoom != 1.0f
                           ? static_cast<float>(degrees(2.0 * std::atan(std::tan(radians(fov) / 2.0) * g_cutscene_zoom)))
                           : fov;
        g_sprite_scale = aspect_ratio() / kDefaultAspect / g_cutscene_zoom;
    }
    mem::write<float>(kFov, fov);
}

void tick_borders(uint32_t now, bool active) {
    if (!g_borders.last_tick) {
        g_borders.last_tick = now;
        return;
    }
    float dt = std::min(static_cast<float>(now - g_borders.last_tick) / 1000.0f, 0.1f);
    g_borders.last_tick = now;
    g_borders.mult = active ? std::min(g_borders.mult + dt / kBorderAnimSeconds, 1.0f)
                            : std::max(g_borders.mult - dt / kBorderAnimSeconds, 0.0f);
}

void draw_rect(GuestState& s, float left, float bottom, float right, float top) {
    static const uint32_t rect = allocator().alloc(16);
    static const uint32_t black = [] {
        uint32_t c = allocator().alloc(4);
        mem::write<uint32_t>(c, 0xff000000u);
        return c;
    }();
    mem::write<float>(rect, left);
    mem::write<float>(rect + 4, bottom);
    mem::write<float>(rect + 8, right);
    mem::write<float>(rect + 12, top);
    guest_call<Conv::cdecl_>(s, kDrawRect, rect, black);
}

void render_border_bars(GuestState& s) {
    float w = static_cast<float>(mem::read<int32_t>(kRsGlobalWidth));
    float h = static_cast<float>(mem::read<int32_t>(kRsGlobalHeight));
    float m = g_borders.mult;
    if (g_borders.has_letterbox) {
        draw_rect(s, 0.0f, g_borders.top * m, w, 0.0f);
        draw_rect(s, 0.0f, h, w, h - g_borders.bottom * m);
    }
    if (g_borders.has_pillar) {
        float pw = g_borders.pillar * m;
        draw_rect(s, 0.0f, 0.0f, pw, h);
        draw_rect(s, w - pw, 0.0f, w, h);
    }
}

void compute_border_geometry(uint32_t camera) {
    float w = static_cast<float>(mem::read<int32_t>(kRsGlobalWidth));
    float h = static_cast<float>(mem::read<int32_t>(kRsGlobalHeight));
    float screen = w / h;
    float content = kDefaultAspect / kVanillaWindowFraction;
    g_borders.has_letterbox = false;
    g_borders.has_pillar = false;
    g_borders.top = g_borders.bottom = g_borders.pillar = 0.0f;
    g_borders.zoom = 1.0f;
    if (screen > content) {
        g_borders.has_pillar = true;
        g_borders.pillar = (w - h * content) * 0.5f;
        g_borders.zoom = kVanillaWindowFraction * screen / kDefaultAspect;
    } else {
        g_borders.has_letterbox = true;
        float target = screen / content;
        g_borders.zoom = std::max(1.0f, target + 1.5f * kVanillaCenterFudgeY * screen / 480.0f *
                                                     (1.0f + kMinTopBarRatio) / (1.0f - kMinTopBarRatio));
        float fraction = target / g_borders.zoom;
        float center = 0.5f - 0.75f * kVanillaCenterFudgeY * screen / (480.0f * g_borders.zoom);
        g_borders.top = std::max(center - fraction * 0.5f, 0.0f) * h;
        g_borders.bottom = std::max(1.0f - center - fraction * 0.5f, 0.0f) * h;
    }
    uint32_t blur = mem::read<uint32_t>(camera + kCameraBlurType);
    if (blur == kBlurNone || blur == kBlurLightScene) {
        mem::write<uint32_t>(camera + kCameraMotionBlurAlpha, 80);
    }
}

Memory* draw_borders(GuestState& s, uint32_t, Memory* m) {
    uint32_t camera = ecx(s);
    uint32_t now = mem::read<uint32_t>(kTimeInMilliseconds);
    bool widescreen = mem::read<uint8_t>(camera + kCameraWideScreenOnField) != 0;
    bool fading = mem::read<uint8_t>(camera + kCameraFading) != 0;
    float fade_alpha = mem::read<float>(camera + kCameraFadeAlpha);
    bool fade_started = !g_borders.was_fading && fading;
    bool faded = !fading && fade_alpha > 50.0f;
    if ((fade_started && (fade_alpha > 50.0f || mem::read<float>(camera + kCameraFadeDuration) == 0.0f)) || faded) {
        g_borders.mult = 0.0f;
    }
    if (!animated_borders()) {
        g_borders.mult = widescreen ? 1.0f : 0.0f;
    } else {
        tick_borders(now, widescreen && !fading && !faded);
    }
    if (widescreen) {
        compute_border_geometry(camera);
    }
    BorderMode mode = border_mode();
    bool visible = g_borders.mult > 0.01f && (widescreen || animated_borders());
    if (mode != BorderMode::off && visible &&
        (mode == BorderMode::both || (mode == BorderMode::letterbox && g_borders.has_letterbox) ||
         (mode == BorderMode::pillarbox && g_borders.has_pillar))) {
        render_border_bars(s);
    }
    g_borders.was_fading = fading;
    g_cutscene_zoom = g_borders.zoom;
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
    return m;
}

uint32_t wheel_cam_fov(GuestState& s, uint32_t) {
    double inverse = 2.0 * std::atan(std::tan(radians(70.0) * 0.5) * (kDefaultAspect / aspect_ratio()));
    mem::write<float>(esi(s) + kCamFov, static_cast<float>(degrees(inverse)));
    return kWheelCamResume;
}

uint32_t skip_border_condition_a(GuestState&, uint32_t) { return kBorderCallA; }

uint32_t skip_border_condition_b(GuestState&, uint32_t) { return kBorderCallB; }

uint32_t help_text_border(GuestState& s, uint32_t) {
    int offset = 0;
    BorderMode mode = border_mode();
    if (mode != BorderMode::off && mode != BorderMode::pillarbox && g_borders.has_letterbox &&
        g_borders.mult > 0.01f) {
        float top = g_borders.top * g_borders.mult;
        if (top > 0.0f) {
            offset = std::min(static_cast<int>(top + 0.5f), 127);
        }
    }
    eax(s) = (eax(s) & 0xffffff00u) | static_cast<uint8_t>(offset);
    return kHelpTextResume;
}

uint32_t frustum_fov(GuestState& s, uint32_t) {
    x87_push(s, radians(g_scaled_fov) / 2.0);
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), esi(s));
    return kFrustumResume;
}

uint32_t target_vector_aspect(GuestState& s, uint32_t pc) {
    x87_set_st0(s, x87_st0(s) * aspect_ratio());
    return pc;
}

uint32_t aim_width_a(GuestState& s, uint32_t) {
    x87_set_st0(s, x87_st0(s) * kCameraWidth);
    return kAimWidthResumeA;
}

uint32_t aim_width_b(GuestState& s, uint32_t) {
    x87_set_st0(s, x87_st0(s) * kCameraWidth);
    return kAimWidthResumeB;
}

uint32_t aim_skip_aspect_divide(GuestState&, uint32_t) { return kAimDivideResume; }

double sky_spread() { return kSkySpread * (aspect_ratio() / kDefaultAspect); }

uint32_t sky_spread_a(GuestState& s, uint32_t) {
    x87_set_st0(s, x87_st0(s) * sky_spread());
    return kSkySpreadResumeA;
}

uint32_t sky_spread_b(GuestState& s, uint32_t) {
    x87_set_st0(s, x87_st0(s) * sky_spread());
    return kSkySpreadResumeB;
}

uint32_t sprite_width_a(GuestState& s, uint32_t) {
    x87_push(s, static_cast<double>(mem::read<int32_t>(kRsGlobalHeight)));
    return kSpriteWidthResumeA;
}

uint32_t sprite_width_b(GuestState& s, uint32_t) {
    x87_push(s, static_cast<double>(mem::read<int32_t>(kRsGlobalHeight)));
    return kSpriteWidthResumeB;
}

uint32_t camera_size(GuestState& s, uint32_t pc) {
    mem::write<float>(esp(s) + 0xc, static_cast<float>(std::tan(radians(g_scaled_fov * 0.5))));
    mem::write<float>(esp(s) + 0x10, aspect_ratio());
    return pc;
}

float hud_percent(const char* key) {
    return std::clamp(static_cast<float>(mod_settings().number(key)), 0.25f, 4.0f);
}

float setting_number(const char* key) { return static_cast<float>(mod_settings().number(key)); }

float radar_width_setting() {
    static const float value = setting_number("widescreen.radar_width");
    return value;
}

float radar_height_setting() {
    static const float value = setting_number("widescreen.radar_height");
    return value;
}

float subtitles_scale() {
    static const float value = std::clamp(setting_number("widescreen.subtitles_scale"), 0.25f, 4.0f);
    return value;
}

float hud_width_scale() {
    static const float scale = hud_percent("widescreen.hud_width");
    return scale;
}

float hud_height_scale() {
    static const float scale = hud_percent("widescreen.hud_height");
    return scale;
}

float narrow(float original) { return original / (screen_aspect() / kDefaultAspect); }

float hud_scale(float original) { return narrow(original); }

float hud_width(float) { return narrow(kHudWidth) * hud_width_scale(); }

float hud_height(float) { return kHudHeight * hud_height_scale(); }

float weapon_icon_width(float) { return narrow(kWeaponIconWidth) * hud_width_scale(); }

float menu_width(float) { return narrow(kHudWidth); }

float menu_height(float) { return kHudHeight; }

float menu_scaled_width(float) { return 480.0f * screen_aspect(); }

float unscaled_width(float) { return kHudWidth; }

float crosshair_height(float) { return 1.0f / 480.0f; }

float lock_on_width(float) { return kLockOnWidth; }

float weapon_icon_left(float) { return kWeaponIconLeft; }

float radar_width(float) { return radar_width_setting(); }

float radar_height(float) { return radar_height_setting(); }

float player_info_bars_x(float) { return kPlayerInfoBarsX; }

float subtitles_width(float) { return kSubtitlesWidth * subtitles_scale(); }

float subtitles_height(float) { return kSubtitlesHeight * subtitles_scale(); }

float subtitles_wrap_reserve(float) {
    return 2.0f * std::max(140.0f, 40.0f + radar_width_setting() + 6.0f) - 20.0f;
}

float subtitles_shift(float) { return 20.0f; }

float sprite_fov_scale(float original) { return original * g_sprite_scale; }

float print_char_min_x(float) {
    float h = static_cast<float>(mem::read<int32_t>(kRsGlobalHeight));
    return (h * kDefaultAspect - static_cast<float>(mem::read<int32_t>(kRsGlobalWidth))) / 2.0f;
}

float zero(float) { return 0.0f; }

float one(float) { return 1.0f; }

float menu_from_right(float x) {
    return static_cast<float>(mem::read<int32_t>(kRsGlobalWidth)) * (1.0f - narrow((640.0f - x) / 640.0f));
}

Memory* print_subtitles(GuestState& s, uint32_t, Memory* m) {
    uint32_t x = mem::read<uint32_t>(esp(s) + 4);
    float y = mem::read<float>(esp(s) + 8);
    uint32_t text = mem::read<uint32_t>(esp(s) + 12);
    if (text) {
        float line_height = 18.0f * mem::read<float>(kFontScaleY);
        auto lines = static_cast<int16_t>(
            guest_call<Conv::cdecl_, uint32_t>(s, kGetNumberLines, x, std::bit_cast<uint32_t>(y), text));
        float bottom = y + static_cast<float>(lines) * line_height;
        float limit = static_cast<float>(mem::read<int32_t>(kRsGlobalHeight)) * (1.0f - 6.0f / 480.0f);
        if (bottom > limit) {
            y -= bottom - limit;
        }
    }
    guest_call<Conv::cdecl_>(s, redirect_original(), x, std::bit_cast<uint32_t>(y), text);
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
    return m;
}

struct WaterPoint {
    double x;
    double y;
};

double water_cross(const WaterPoint& a, const WaterPoint& b, const WaterPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

struct WaterPolygon {
    std::array<WaterPoint, 10> points{};
    int count = 0;
};

WaterPolygon water_hull(std::array<WaterPoint, 5> points) {
    for (const auto& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            return {};
        }
    }
    std::ranges::sort(points, [](const auto& a, const auto& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    auto end = std::unique(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.x == b.x && a.y == b.y;
    });
    int size = static_cast<int>(end - points.begin());
    if (size < 3) {
        return {};
    }
    WaterPolygon hull;
    int& count = hull.count;
    for (int i = 0; i < size; ++i) {
        while (count >= 2 && water_cross(hull.points[count - 2], hull.points[count - 1], points[i]) <= 0.0) {
            --count;
        }
        hull.points[count++] = points[i];
    }
    int lower = count;
    for (int i = size - 2; i >= 0; --i) {
        while (count > lower && water_cross(hull.points[count - 2], hull.points[count - 1], points[i]) <= 0.0) {
            --count;
        }
        hull.points[count++] = points[i];
    }
    --count;
    if (count < 3) {
        return {};
    }
    return hull;
}

bool water_cell_intersects(const WaterPolygon& polygon, int x, int y) {
    for (int i = 0; i < polygon.count; ++i) {
        const auto& a = polygon.points[i];
        const auto& b = polygon.points[(i + 1) % polygon.count];
        WaterPoint corner{static_cast<float>(x + (b.y < a.y)), static_cast<float>(y + (b.x > a.x))};
        if (water_cross(a, b, corner) < -0.000001) {
            return false;
        }
    }
    return true;
}

void return_from_redirect(GuestState& s) {
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4;
}

Memory* scan_water(GuestState& s, uint32_t, Memory* m) {
    uint32_t points = mem::read<uint32_t>(esp(s) + 4);
    int32_t count = mem::read<int32_t>(esp(s) + 8);
    uint32_t callback = mem::read<uint32_t>(esp(s) + 12);
    auto original = [&] {
        eax(s) = guest_call<Conv::cdecl_, uint32_t>(s, redirect_original(), points, static_cast<uint32_t>(count),
                                                    callback);
        return_from_redirect(s);
        return m;
    };
    if (count != 5) {
        return original();
    }
    std::array<WaterPoint, 5> input{};
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = {mem::read<float>(points + static_cast<uint32_t>(i) * 8),
                    mem::read<float>(points + static_cast<uint32_t>(i) * 8 + 4)};
    }
    WaterPolygon polygon = water_hull(input);
    if (polygon.count < 3) {
        return original();
    }
    float min_x = static_cast<float>(input[0].x), max_x = min_x;
    float min_y = static_cast<float>(input[0].y), max_y = min_y;
    for (const auto& p : input) {
        min_x = std::min(min_x, static_cast<float>(p.x));
        max_x = std::max(max_x, static_cast<float>(p.x));
        min_y = std::min(min_y, static_cast<float>(p.y));
        max_y = std::max(max_y, static_cast<float>(p.y));
    }
    if (min_x < -32768.0f || min_y < -32768.0f || max_x >= 32767.0f || max_y >= 32767.0f) {
        return original();
    }
    for (int x = static_cast<int>(std::floor(min_x)); x <= static_cast<int>(std::floor(max_x)); ++x) {
        for (int y = static_cast<int>(std::floor(min_y)); y <= static_cast<int>(std::floor(max_y)); ++y) {
            if (water_cell_intersects(polygon, x, y)) {
                guest_call<Conv::cdecl_>(s, callback, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            }
        }
    }
    eax(s) = 0;
    return_from_redirect(s);
    return m;
}

constexpr NativeSymbol kSymbols[] = {
    {"ws_scan_water", scan_water},
    {"ws_print_subtitles", print_subtitles},
    {"ws_calculate_aspect_ratio", native_trace<Conv::cdecl_, calculate_aspect_ratio>},
    {"ws_menu_from_right", native_trace<Conv::stdcall, menu_from_right>},
    {"ws_draw_borders", draw_borders},
    {"ws_set_fov", native_trace<Conv::cdecl_, set_fov>},
};

constexpr MidHookSymbol kMidHooks[] = {
    {"ws_frustum_fov", frustum_fov},
    {"ws_target_vector_aspect", target_vector_aspect},
    {"ws_aim_width_a", aim_width_a},
    {"ws_aim_width_b", aim_width_b},
    {"ws_aim_skip_aspect_divide", aim_skip_aspect_divide},
    {"ws_camera_size", camera_size},
    {"ws_sky_spread_a", sky_spread_a},
    {"ws_sky_spread_b", sky_spread_b},
    {"ws_sprite_width_a", sprite_width_a},
    {"ws_sprite_width_b", sprite_width_b},
    {"ws_skip_border_condition_a", skip_border_condition_a},
    {"ws_skip_border_condition_b", skip_border_condition_b},
    {"ws_help_text_border", help_text_border},
    {"ws_wheel_cam_fov", wheel_cam_fov},
};

constexpr SlotSymbol kSlots[] = {
    {"ws_hud_scale", hud_scale},
    {"ws_hud_width", hud_width},
    {"ws_hud_height", hud_height},
    {"ws_weapon_icon_width", weapon_icon_width},
    {"ws_menu_width", menu_width},
    {"ws_menu_height", menu_height},
    {"ws_menu_scaled_width", menu_scaled_width},
    {"ws_unscaled_width", unscaled_width},
    {"ws_crosshair_height", crosshair_height},
    {"ws_lock_on_width", lock_on_width},
    {"ws_weapon_icon_left", weapon_icon_left},
    {"ws_radar_width", radar_width},
    {"ws_radar_height", radar_height},
    {"ws_player_info_bars_x", player_info_bars_x},
    {"ws_subtitles_width", subtitles_width},
    {"ws_subtitles_height", subtitles_height},
    {"ws_subtitles_wrap_reserve", subtitles_wrap_reserve},
    {"ws_subtitles_shift", subtitles_shift},
    {"ws_sprite_fov_scale", sprite_fov_scale},
    {"ws_print_char_min_x", print_char_min_x},
    {"ws_zero", zero},
    {"ws_one", one},
};

}

std::span<const SlotSymbol> widescreen_slots() { return kSlots; }

std::span<const NativeSymbol> widescreen_symbols() { return kSymbols; }

std::span<const MidHookSymbol> widescreen_mid_hooks() { return kMidHooks; }

}
