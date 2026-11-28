#include "natives.hpp"

#include "../mod_settings.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ma::host::native {

namespace {

constexpr uint32_t kGetLookBehindForCar = 0x0053fe70;
constexpr uint32_t kJetpackHover = 0x0067edaf;
constexpr uint32_t kWorldRemove = 0x00563280;
constexpr uint32_t kEntityIsVisible = 0x00536bc0;
constexpr uint32_t kObjectPool = 0x00b7449c;
constexpr uint32_t kFindPlayerPed = 0x0056e210;
constexpr uint32_t kRebuildPlayer = 0x005a82c0;
constexpr uint32_t kGetAnimationBlockIndex = 0x004d3990;
constexpr uint32_t kRequestModel = 0x004087e0;
constexpr uint32_t kLoadAllRequestedModels = 0x0040ea10;
constexpr uint32_t kAnimGroupIdOffsetOperand = 0x005a814e;
constexpr uint32_t kIsTrainModel = 0x004c5ad0;
constexpr uint32_t kTrainCtor = 0x006f6030;
constexpr uint32_t kWaitForSingleObjectIat = 0x00858088;
constexpr uint32_t kReleaseSemaphoreIat = 0x0085808c;

uint32_t guest_literal(std::string_view text) {
    uint32_t addr = allocator().alloc(static_cast<uint32_t>(text.size() + 1));
    mem::write_cstr(addr, text);
    return addr;
}

void return_to_caller(GuestState& s, uint32_t stack_bytes = 0) {
    eip(s) = mem::read<uint32_t>(esp(s));
    esp(s) += 4 + stack_bytes;
}

uint32_t stack_arg(GuestState& s, unsigned index) { return mem::read<uint32_t>(esp(s) + 4 + 4 * index); }

void log_once(std::atomic<bool>& flag, const char* text) {
    if (!flag.exchange(true)) {
        std::fprintf(stderr, "[SP] %s\n", text);
    }
}

struct SavedRegs {
    explicit SavedRegs(GuestState& s) : s_(s), ebx_(ebx(s)), esi_(esi(s)), edi_(edi(s)), ebp_(ebp(s)) {}
    ~SavedRegs() {
        ebx(s_) = ebx_;
        esi(s_) = esi_;
        edi(s_) = edi_;
        ebp(s_) = ebp_;
    }
    GuestState& s_;
    uint32_t ebx_, esi_, edi_, ebp_;
};

void delete_entity(GuestState& s, uint32_t entity) {
    uint32_t vtable = mem::read<uint32_t>(entity);
    guest_call<Conv::thiscall>(s, mem::read<uint32_t>(vtable), entity, 1u);
}

Memory* coldata_malloc_init(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t col = guest_call<Conv::cdecl_, uint32_t>(s, redirect_original(), stack_arg(s, 0), stack_arg(s, 1));
    if (col) {
        mem::write<uint8_t>(col + 0x07, 0);
        mem::write<uint32_t>(col + 0x20, 0);
        mem::write<uint32_t>(col + 0x24, 0);
        mem::write<uint32_t>(col + 0x28, 0);
        mem::write<uint32_t>(col + 0x2c, 0);
        log_once(reached, "collision data: new CColData flags and shadow mesh cleared");
    }
    eax(s) = col;
    return_to_caller(s);
    return m;
}

Memory* coldata_new_init(GuestState& s, uint32_t, Memory* m) {
    uint32_t col = guest_call<Conv::cdecl_, uint32_t>(s, redirect_original(), stack_arg(s, 0));
    if (col) {
        mem::write<uint8_t>(col + 0x07, 0);
    }
    eax(s) = col;
    return_to_caller(s);
    return m;
}

std::vector<uint32_t>& escalator_steps_to_remove() {
    static std::vector<uint32_t> steps;
    return steps;
}

Memory* escalator_switch_off(GuestState& s, uint32_t, Memory* m) {
    uint32_t escalator = ecx(s);
    if (mem::read<uint8_t>(escalator + 0x79)) {
        int32_t count = static_cast<int32_t>(mem::read<uint32_t>(escalator + 0x7c) + mem::read<uint32_t>(escalator + 0x80) +
                                             mem::read<uint32_t>(escalator + 0x84));
        for (int32_t i = 0; i < count && i < 42; ++i) {
            uint32_t slot = escalator + 0xa8 + 4 * static_cast<uint32_t>(i);
            if (uint32_t step = mem::read<uint32_t>(slot)) {
                escalator_steps_to_remove().push_back(step);
                mem::write<uint32_t>(slot, 0);
            }
        }
        mem::write<uint8_t>(escalator + 0x79, 0);
    }
    return_to_caller(s);
    return m;
}

uint32_t escalators_update(GuestState& s, uint32_t pc) {
    auto& steps = escalator_steps_to_remove();
    if (steps.empty()) {
        return pc;
    }
    static std::atomic<bool> reached = false;
    log_once(reached, "escalators: removing switched-off steps before the update");
    SavedRegs saved(s);
    std::vector<uint32_t> pending;
    pending.swap(steps);
    for (uint32_t step : pending) {
        guest_call<Conv::cdecl_>(s, kWorldRemove, step);
        delete_entity(s, step);
    }
    return pc;
}

std::pair<int32_t, int32_t> free_temp_objects(GuestState& s, int32_t wanted, bool force) {
    uint32_t pool = mem::read<uint32_t>(kObjectPool);
    uint32_t slots = mem::read<uint32_t>(pool);
    uint32_t infos = mem::read<uint32_t>(pool + 4);
    int32_t count = static_cast<int32_t>(mem::read<uint32_t>(pool + 8));
    int32_t processed = 0;
    int32_t freed = 0;
    for (int32_t i = 0; i < count && freed < wanted; ++i) {
        if (mem::read<uint8_t>(infos + static_cast<uint32_t>(i)) & 0x80) {
            continue;
        }
        uint32_t object = slots + 0x17c * static_cast<uint32_t>(i);
        if (mem::read<uint8_t>(object + 0x13c) != 3) {
            continue;
        }
        ++processed;
        if (force || !guest_call<Conv::thiscall, uint8_t>(s, kEntityIsVisible, object)) {
            ++freed;
            guest_call<Conv::cdecl_>(s, kWorldRemove, object);
            delete_entity(s, object);
        }
    }
    return {processed, freed};
}

Memory* try_to_free_up_temp_objects(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> forced = false;
    SavedRegs saved(s);
    int32_t wanted = static_cast<int32_t>(stack_arg(s, 0));
    auto [processed, freed] = free_temp_objects(s, wanted, false);
    if (freed < wanted) {
        auto [unused, forced_count] = free_temp_objects(s, std::min(processed, wanted - freed), true);
        if (forced_count) {
            log_once(forced, "temp objects: visible temp objects freed to make room");
        }
    }
    return_to_caller(s);
    return m;
}

uint32_t grab_screen_release_surface(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    uint32_t surface = mem::read<uint32_t>(esp(s) + 4);
    if (!surface) {
        return pc;
    }
    log_once(reached, "photo: psGrabScreen unlocks and releases the back buffer surface");
    uint32_t result = eax(s);
    uint32_t vtable = mem::read<uint32_t>(surface);
    guest_call<Conv::stdcall>(s, mem::read<uint32_t>(vtable + 0x38), surface);
    uint32_t refs = guest_call<Conv::stdcall, uint32_t>(s, mem::read<uint32_t>(vtable + 0x08), surface);
    std::fprintf(stderr, "[SP] photo: back buffer surface 0x%08x released, %u references left\n", surface, refs);
    eax(s) = result;
    return pc;
}

Memory* restore_replay_rebuild_player(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    guest_call<Conv::cdecl_>(s, redirect_original());
    uint32_t player = guest_call<Conv::cdecl_, uint32_t>(s, kFindPlayerPed, 0xffffffffu);
    if (player) {
        guest_call<Conv::cdecl_>(s, kRebuildPlayer, player, 0u);
        log_once(reached, "replay: player rebuilt after restoring the game state");
    }
    return_to_caller(s);
    return m;
}

Memory* rebuild_player_load_motion_groups(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    static const uint32_t fat = guest_literal("fat");
    static const uint32_t muscular = guest_literal("muscular");
    guest_call<Conv::cdecl_>(s, redirect_original(), stack_arg(s, 0), stack_arg(s, 1));
    int32_t offset = static_cast<int32_t>(mem::read<uint32_t>(kAnimGroupIdOffsetOperand));
    for (uint32_t name : {fat, muscular}) {
        int32_t block = static_cast<int32_t>(guest_call<Conv::cdecl_, uint32_t>(s, kGetAnimationBlockIndex, name));
        guest_call<Conv::cdecl_>(s, kRequestModel, static_cast<uint32_t>(block + offset), 18u);
    }
    guest_call<Conv::cdecl_>(s, kLoadAllRequestedModels, 1u);
    log_once(reached, "replay: fat and muscular motion groups loaded with the player");
    return_to_caller(s);
    return m;
}

Memory* vehicles_ide_defaults(GuestState& s, uint32_t, Memory* m) {
    mem::write<uint32_t>(stack_arg(s, 13), 0xffffffffu);
    mem::write<float>(stack_arg(s, 14), 0.7f);
    mem::write<float>(stack_arg(s, 15), 0.7f);
    mem::write<uint32_t>(stack_arg(s, 16), 0xffffffffu);
    std::array<uint32_t, 17> args{};
    for (unsigned i = 0; i < args.size(); ++i) {
        args[i] = stack_arg(s, i);
    }
    uint32_t fields = call_guest(s, redirect_original(), std::span<const uint32_t>(args), false);
    if (fields < 15) {
        std::fprintf(stderr, "[SP] vehicles.ide: %s has %u of 15 fields; wheel model %d, sizes %.2f %.2f, upgrade class %d\n",
                     mem::read_cstr(args[3], 24).c_str(), fields, static_cast<int32_t>(mem::read<uint32_t>(args[13])),
                     mem::read<float>(args[14]), mem::read<float>(args[15]), static_cast<int32_t>(mem::read<uint32_t>(args[16])));
    }
    eax(s) = fields;
    return_to_caller(s);
    return m;
}

Memory* automobile_ctor_or_train(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t object = ecx(s);
    uint32_t model = stack_arg(s, 0);
    uint32_t created_by = stack_arg(s, 1) & 0xff;
    if (!guest_call<Conv::cdecl_, uint8_t>(s, kIsTrainModel, model)) {
        ecx(s) = object;
        eip(s) = redirect_original();
        return m;
    }
    uint32_t train = guest_call<Conv::thiscall, uint32_t>(s, kTrainCtor, object, model, created_by);
    mem::write<uint8_t>(train + 0x5b9, mem::read<uint8_t>(train + 0x5b9) | 0x01);
    uint32_t flags = mem::read<uint32_t>(train + 0x40);
    flags &= ~((1u << 2) | (1u << 17));
    mem::write<uint32_t>(train + 0x40, flags);
    log_once(reached, "garage: stored train restored as a derailed train");
    eax(s) = train;
    return_to_caller(s, 12);
    return m;
}

Memory* timer_ftol(GuestState& s, uint32_t, Memory* m) {
    static std::mutex lock;
    static std::map<uint32_t, double> remainders;
    double value = x87_pop(s);
    int32_t whole;
    {
        std::lock_guard guard(lock);
        double& rest = remainders[redirect_site()];
        double integral;
        rest = std::modf(value + rest, &integral);
        whole = static_cast<int32_t>(integral);
    }
    eax(s) = static_cast<uint32_t>(whole);
    edx(s) = whole < 0 ? 0xffffffffu : 0u;
    return_to_caller(s);
    return m;
}

uint32_t zone_locked_or_unlocked(float x, float y) {
    float grid_offset_x = mem::read<float>(mem::read<uint32_t>(0x00572137));
    float grid_offset_y = mem::read<float>(mem::read<uint32_t>(0x0057214c));
    float grid_size_x = mem::read<float>(mem::read<uint32_t>(0x0057213d));
    float grid_size_y = mem::read<float>(mem::read<uint32_t>(0x00572155));
    int32_t count_x = static_cast<int32_t>((2.0f * grid_offset_x) * grid_size_x);
    int32_t count_y = static_cast<int32_t>((2.0f * grid_offset_y) * grid_size_y);
    uint32_t visited = mem::read<uint32_t>(0x0057216a) - static_cast<uint32_t>(count_y - 1);
    int32_t ix = static_cast<int32_t>((x + grid_offset_x) * grid_size_x);
    int32_t iy = static_cast<int32_t>((y + grid_offset_y) * grid_size_y);
    if (ix >= 0 && ix < count_x && iy >= 0 && iy < count_y) {
        return mem::read<uint8_t>(visited + static_cast<uint32_t>(count_x * ix - iy + (count_y - 1))) != 0;
    }
    static std::atomic<bool> outside = false;
    log_once(outside, "zones: position outside the map counts as unlocked");
    return 1;
}

uint32_t player_ped_data_assignment(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    log_once(reached, "replay: CPlayerPedData copy keeps all flag bits");
    uint32_t dst = eax(s) + 0x34;
    uint32_t src = ecx(s) + 0x34;
    uint32_t d = edx(s);
    d ^= mem::read<uint32_t>(src);
    d &= 1;
    mem::write<uint32_t>(dst, mem::read<uint32_t>(dst) ^ d);
    uint32_t e = mem::read<uint32_t>(dst);
    bool use_edx = true;
    for (uint32_t bit = 2; bit <= 0x100; bit <<= 1) {
        uint32_t v = mem::read<uint32_t>(src);
        uint32_t prev = use_edx ? e : d;
        v = ((v ^ prev) & bit) ^ prev;
        mem::write<uint32_t>(dst, v);
        if (use_edx) {
            d = v;
        } else {
            e = v;
        }
        use_edx = !use_edx;
    }
    edx(s) = mem::read<uint32_t>(src);
    esi(s) = e;
    return pc;
}

constexpr uint32_t kScriptSpaceOperand = 0x005d5380;
constexpr uint32_t kScriptParamsOperand = 0x0048995b;
constexpr uint32_t kScriptFileSizeOperand = 0x00468e75;
constexpr uint32_t kScriptMissionSizeOperand = 0x00489a5b;
constexpr uint32_t kStatTypesIntOperand = 0x0055c0d8;

struct ScriptSpace {
    uint32_t base;
    uint32_t params;
    uint32_t main_size;
    uint32_t mission_size;
};

ScriptSpace script_space() {
    return {mem::read<uint32_t>(kScriptSpaceOperand), mem::read<uint32_t>(kScriptParamsOperand),
            mem::read<uint32_t>(kScriptFileSizeOperand), mem::read<uint32_t>(kScriptMissionSizeOperand)};
}

std::vector<std::optional<uint8_t>> parse_pattern(std::string_view text) {
    std::vector<std::optional<uint8_t>> out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        if (text[i] == '?') {
            out.emplace_back();
            ++i;
            continue;
        }
        out.emplace_back(static_cast<uint8_t>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
        i += 2;
    }
    return out;
}

std::vector<uint32_t> find_pattern(uint32_t begin, uint32_t end, std::string_view text) {
    auto pattern = parse_pattern(text);
    std::vector<uint32_t> hits;
    if (pattern.empty() || end < begin + pattern.size()) {
        return hits;
    }
    const uint8_t* base = mem::ptr(0);
    for (uint32_t a = begin; a + pattern.size() <= end; ++a) {
        bool match = true;
        for (size_t k = 0; k < pattern.size() && match; ++k) {
            match = !pattern[k] || base[a + k] == *pattern[k];
        }
        if (match) {
            hits.push_back(a);
        }
    }
    return hits;
}

void write_bytes(uint32_t addr, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) {
        mem::write<uint8_t>(addr++, b);
    }
}

void basketball_fix(uint32_t buf, int32_t size) {
    struct Fix {
        uint32_t from;
        float to;
    };
    static constexpr Fix kFixes[] = {
        {0x449de19a, 1291.8f}, {0xc4416ae1, -797.8284f}, {0x44886c7b, 1089.5f}, {0x449df852, 1286.8f},
        {0xc44225c3, -797.69f}, {0x44885c7b, 1089.1f},   {0x43373ae1, 90.0f},
    };
    int hits = 0;
    for (int32_t i = 0; i < size && hits < 7; ++i) {
        uint32_t value = mem::read<uint32_t>(buf + static_cast<uint32_t>(i));
        for (const auto& f : kFixes) {
            if (value == f.from) {
                mem::write<float>(buf + static_cast<uint32_t>(i), f.to);
                ++hits;
                break;
            }
        }
    }
    if (hits) {
        std::fprintf(stderr, "[SP] script: basketball save point moved (%d values)\n", hits);
    }
}

void quadruple_stunt_bonus(const ScriptSpace& sc) {
    auto hits = find_pattern(sc.base, sc.base + sc.main_size, "20 00 02 60 14 06 00 00 80 40");
    if (hits.size() == 1) {
        write_bytes(hits[0], {0x18, 0x00, 0x02, 0x30, 0x14, 0x01, 0x04, 0x00, 0x00, 0x00});
        std::fprintf(stderr, "[SP] script: quadruple stunt bonus compares the integer height\n");
    }
}

std::vector<uint32_t> mission_pattern(const ScriptSpace& sc, std::string_view text) {
    uint32_t begin = sc.base + sc.main_size;
    return find_pattern(begin, begin + sc.mission_size, text);
}

void sweets_girl_fix(const ScriptSpace& sc) {
    for (uint32_t off : {2510u, 2680u}) {
        uint32_t at = sc.base + sc.main_size + off;
        if (mem::read<uint16_t>(at) == 0x0039) {
            mem::write<uint16_t>(at, 0x0029);
        }
    }
}

void mountain_cloud_boys_fix(const ScriptSpace& sc) {
    auto hits = mission_pattern(sc, "D6 00 04 00 39 00 03 EF 00 04 02 4D 00 01 90 F2 FF FF D6 00 04 01");
    if (hits.size() == 1) {
        write_bytes(hits[0], {0x00, 0x00, 0x00, 0x00, 0xD6, 0x00, 0x04, 0x03, 0x39, 0x00, 0x03, 0x2B, 0x00, 0x04, 0x0B, 0x39,
                              0x00, 0x03, 0xEF, 0x00, 0x04, 0x02});
    }
}

void ice_cold_killa_fix(const ScriptSpace& sc) {
    auto hits = mission_pattern(sc, "06 B9 A6 27 45");
    if (hits.size() == 1) {
        mem::write<float>(hits[0] + 1, -mem::read<float>(hits[0] + 1));
    }
}

void supply_lines_fix(const ScriptSpace& sc, bool beefy_baron) {
    auto hits = mission_pattern(sc, beefy_baron ? "06 B8 9E 3A 44" : "06 B8 1E 2F 44");
    if (hits.size() == 1) {
        mem::write<float>(hits[0] + 1, 10.0f);
    }
}

void school_cones_fix(const ScriptSpace& sc, bool driving) {
    auto hits = mission_pattern(
        sc, driving ? "04 00 02 20 03 04 00 D6 00 04 00 1A 00 04 2E 02 20 03 4D 00 01 60 75 FF FF BE 00 08 01 07 24 03 20 03 2E 80 08 00 02 20 03 04 01"
                    : "04 00 02 20 03 04 00 D6 00 04 00 1A 00 04 2E 02 20 03 4D 00 01 F8 AD FF FF 08 01 07 24 03 20 03 2E 80 08 00 02 20 03 04 01");
    std::vector<uint32_t> coil;
    std::vector<uint32_t> burn;
    if (driving) {
        coil = mission_pattern(sc, "1A 00 04 17 02 20 03");
        burn = mission_pattern(sc, "1A 00 04 23 02 20 03");
        if (coil.size() != 1 || burn.size() != 1) {
            return;
        }
    }
    if (hits.size() != 1) {
        return;
    }
    uint32_t at = hits[0];
    if (driving) {
        write_bytes(at, {0x02, 0x00, 0x01, 0x8B, 0x75, 0xFF, 0xFF});
    } else {
        write_bytes(at, {0x02, 0x00, 0x01, 0x21, 0xAE, 0xFF, 0xFF});
    }
    write_bytes(at + 11, {0x18, 0x00, 0x02, 0x20, 0x03, 0x04, 0x00});
    write_bytes(at + (driving ? 0x1b : 0x19),
                {0x0C, 0x00, 0x02, 0x20, 0x03, 0x04, 0x01, 0x08, 0x01, 0x07, 0x24, 0x03, 0x20, 0x03, 0x2E, 0x80});
    mem::write<int32_t>(sc.base + 800, 0);
    if (driving) {
        mem::write<int8_t>(coil[0] + 3, 30);
        mem::write<int8_t>(burn[0] + 3, 42);
    }
}

void air_raid_fix(const ScriptSpace& sc) {
    auto gosub_place = mission_pattern(sc, "BD 01 03 65 01 06 00 03 48 01 04 00");
    auto free_space = mission_pattern(sc, "EF 04 0E 06 43 41 53 49 4E 4F EF 04 0E 0A 4F 4E 5F 4C 4F 4F 4B 45 52 53 D8 00 51 00");
    if (gosub_place.size() != 1 || free_space.size() != 1) {
        return;
    }
    uint32_t mission_base = sc.base + sc.main_size;
    uint32_t out = free_space[0] + 28;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        write_bytes(out, bytes);
        out += static_cast<uint32_t>(bytes.size());
    };
    auto emit_i32 = [&](int32_t v) {
        mem::write<int32_t>(out, v);
        out += 4;
    };
    uint32_t init = gosub_place[0] + 5;
    write_bytes(init, {0x50, 0x00, 0x01});
    mem::write<int32_t>(init + 3, static_cast<int32_t>(mission_base - out));
    emit({0x06, 0x00, 0x03, 0x48, 0x01, 0x04, 0x00});
    emit({0xB8, 0x04, 0x02, 0x0C, 0x00, 0x04, 0x08, 0x03, 0x72, 0x01, 0x03, 0x73, 0x01, 0x03, 0x74, 0x01});
    emit({0x51, 0x00});
    uint32_t cleanup_gosub = mission_base + 0x1e;
    int32_t original_cleanup = mem::read<int32_t>(cleanup_gosub);
    mem::write<int32_t>(cleanup_gosub, static_cast<int32_t>(mission_base - out));
    emit({0x12, 0x81});
    emit({0x4D, 0x00, 0x01});
    emit_i32(original_cleanup);
    emit({0x47, 0x02, 0x03, 0x74, 0x01});
    emit({0x8B, 0x03});
    emit({0xB2, 0x01, 0x02, 0x0C, 0x00, 0x03, 0x72, 0x01, 0x03, 0x73, 0x01});
    emit({0x49, 0x02, 0x03, 0x74, 0x01});
    emit({0x02, 0x00, 0x01});
    emit_i32(original_cleanup);
}

Memory* scripts_load_basketball_fix(GuestState& s, uint32_t, Memory* m) {
    guest_call<Conv::cdecl_>(s, redirect_original());
    auto sc = script_space();
    basketball_fix(sc.base + 8, static_cast<int32_t>(mem::read<uint32_t>(sc.base + 3)));
    quadruple_stunt_bonus(sc);
    return_to_caller(s);
    return m;
}

Memory* start_mission_script_fixes(GuestState& s, uint32_t, Memory* m) {
    guest_call<Conv::cdecl_>(s, redirect_original());
    auto sc = script_space();
    int32_t mission = static_cast<int32_t>(mem::read<uint32_t>(sc.params));
    switch (mission) {
    case 0:
        basketball_fix(sc.base + sc.main_size, static_cast<int32_t>(sc.mission_size));
        quadruple_stunt_bonus(sc);
        break;
    case 18:
        sweets_girl_fix(sc);
        break;
    case 53:
        mountain_cloud_boys_fix(sc);
        break;
    case 61:
        ice_cold_killa_fix(sc);
        break;
    case 71:
        school_cones_fix(sc, true);
        break;
    case 120:
        school_cones_fix(sc, false);
        break;
    case 72:
        air_raid_fix(sc);
        break;
    case 73:
        supply_lines_fix(sc, false);
        break;
    case 10:
        supply_lines_fix(sc, true);
        break;
    default:
        break;
    }
    std::fprintf(stderr, "[SP] script: mission %d started with script fixes\n", mission);
    return_to_caller(s);
    return m;
}

uint32_t gym_glitch_get_date(GuestState& s, uint32_t) {
    uint32_t script = ebp(s);
    std::string name = mem::read_cstr(script + 8, 8);
    if (name == "gymbike" || name == "gymbenc" || name == "gymtrea" || name == "gymdumb") {
        uint32_t stats = mem::read<uint32_t>(kStatTypesIntOperand);
        edx(s) = mem::read<uint32_t>(stats + 4 * (134 - 120)) + 1;
        eax(s) = 0xffffffffu;
        static std::atomic<bool> reached = false;
        log_once(reached, "script: gym scripts read the workout day stat instead of the calendar");
    } else {
        edx(s) = mem::read<uint8_t>(0x00b70154);
        eax(s) = mem::read<uint8_t>(0x00b70155);
    }
    return 0x00470b11;
}

constexpr uint32_t kRwEngineInstance = 0x00c97b24;
constexpr uint32_t kRsMaximumWidth = 0x00c17044;
constexpr uint32_t kRsMaximumHeight = 0x00c17048;
constexpr uint32_t kFlushSpriteBuffer = 0x0070cf20;
constexpr uint32_t kRemoveFromInterestingVehicleList = 0x00423ed0;
constexpr uint32_t kRwRasterCreate = 0x007fb230;
constexpr uint32_t kFxQuality = 0x00a9ae00 + 0x54;
constexpr uint32_t kMsaaValues = 0x00c9c050;
constexpr uint32_t kMirrorBuffer = 0x00c7c71c;
constexpr uint32_t kMirrorZBuffer = 0x00c7c720;
constexpr uint32_t kStencilShadowPads = 0x00c6a170;

struct SavedScratch {
    explicit SavedScratch(GuestState& s) : s_(s), eax_(eax(s)), ecx_(ecx(s)), edx_(edx(s)), regs_(s) {}
    ~SavedScratch() {
        eax(s_) = eax_;
        ecx(s_) = ecx_;
        edx(s_) = edx_;
    }
    GuestState& s_;
    uint32_t eax_, ecx_, edx_;
    SavedRegs regs_;
};

uint32_t lens_flush_before_ztest(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    log_once(reached, "lens flare: sprite buffer flushed before the Z test switches");
    SavedScratch saved(s);
    guest_call<Conv::cdecl_>(s, kFlushSpriteBuffer);
    return pc;
}

uint32_t lens_init_after_ztest(GuestState&, uint32_t pc) {
    uint32_t camera = mem::read<uint32_t>(0x00c1703c);
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    double near_plane = mem::read<float>(camera + 0x80);
    mem::write<float>(0x00c4b8d0, static_cast<float>(mem::read<float>(0x00858624) / near_plane));
    mem::write<float>(0x00c4b8d8, mem::read<float>(engine + 0x18));
    mem::write<float>(0x00c4b8d4, mem::read<float>(engine + 0x1c));
    return pc;
}

float stuff2d_width() {
    return static_cast<float>(static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumWidth))) *
           mem::read<float>(mem::read<uint32_t>(0x0053e474));
}

float stuff2d_height() {
    return static_cast<float>(static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumHeight))) *
           mem::read<float>(mem::read<uint32_t>(0x0053e3e9));
}

Memory* corona_flare_scale(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t w = esp(s) + 4 + 4 * 2;
    uint32_t h = esp(s) + 4 + 4 * 3;
    mem::write<float>(w, mem::read<float>(w) * stuff2d_width());
    mem::write<float>(h, mem::read<float>(h) * stuff2d_height());
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] corona flares: sprite size scaled by %.3f x %.3f\n", stuff2d_width(), stuff2d_height());
    }
    eip(s) = redirect_original();
    return m;
}

Memory* anim_set_current_anim(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    log_once(reached, "anim: interpolator set up without writing key frame pointers past its frames");
    SavedRegs saved(s);
    uint32_t interp = stack_arg(s, 0);
    uint32_t anim = stack_arg(s, 1);
    mem::write<uint32_t>(interp + 0x00, anim);
    mem::write<float>(interp + 0x04, 0.0f);
    uint32_t info = mem::read<uint32_t>(anim);
    mem::write<uint32_t>(interp + 0x24, mem::read<uint32_t>(info + 0x04));
    mem::write<uint32_t>(interp + 0x28, mem::read<uint32_t>(info + 0x08));
    mem::write<uint32_t>(interp + 0x3c, mem::read<uint32_t>(info + 0x0c));
    mem::write<uint32_t>(interp + 0x40, mem::read<uint32_t>(info + 0x10));
    mem::write<uint32_t>(interp + 0x44, mem::read<uint32_t>(info + 0x14));
    mem::write<uint32_t>(interp + 0x48, mem::read<uint32_t>(info + 0x18));
    int32_t nodes = static_cast<int32_t>(mem::read<uint32_t>(interp + 0x2c));
    uint32_t interp_size = mem::read<uint32_t>(interp + 0x24);
    uint32_t key_size = mem::read<uint32_t>(interp + 0x28);
    uint32_t frames = mem::read<uint32_t>(anim + 0x10);
    uint32_t custom = mem::read<uint32_t>(anim + 0x14);
    uint32_t interpolate = mem::read<uint32_t>(interp + 0x44);
    for (int32_t i = 0; i < nodes; ++i) {
        uint32_t ui = static_cast<uint32_t>(i);
        guest_call<Conv::cdecl_>(s, interpolate, interp + 0x4c + ui * interp_size, frames + ui * key_size,
                                 frames + (ui + static_cast<uint32_t>(nodes)) * key_size, 0.0f, custom);
    }
    mem::write<uint32_t>(interp + 0x08, frames + 2 * key_size * static_cast<uint32_t>(nodes));
    eax(s) = 1;
    return_to_caller(s);
    return m;
}

Memory* record_vehicle_deleted(GuestState& s, uint32_t, Memory* m) {
    uint32_t vehicle = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, redirect_original(), vehicle);
    guest_call<Conv::cdecl_>(s, kRemoveFromInterestingVehicleList, vehicle);
    static std::atomic<bool> reached = false;
    log_once(reached, "impound: deleted vehicle removed from the interesting vehicle list");
    return_to_caller(s);
    return m;
}

Memory* interesting_and_impoundable(GuestState& s, uint32_t, Memory* m) {
    uint32_t vehicle = stack_arg(s, 0);
    uint32_t cls = mem::read<uint32_t>(vehicle + 0x590);
    uint32_t sub = mem::read<uint32_t>(vehicle + 0x594);
    bool car = cls == 0 && sub != 3 && sub != 4 && sub != 11;
    bool bike = cls == 9;
    if (!car && !bike) {
        static std::atomic<bool> refused = false;
        log_once(refused, "impound: helicopters, planes, boats, trains and trailers are not impounded");
        eax(s) = 0;
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* timecyc_line(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t ambient = allocator().alloc(12);
    static const uint32_t fallback_format = guest_literal(
        "%*d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %f %f %f %d %d %d %f %f %f %d %d %d %d %d %d %f %f %f %f %f "
        "%f %f %f %f %f %f %f %f %d %d %f");
    static int32_t last[3] = {};
    static std::atomic<int> fallbacks = 0;
    SavedRegs saved(s);
    std::array<uint32_t, 54> args{};
    for (unsigned i = 0; i < args.size(); ++i) {
        args[i] = stack_arg(s, i);
    }
    mem::write<float>(args[53], 1.0f);
    std::array<uint32_t, 54> call = args;
    for (unsigned k = 0; k < 3; ++k) {
        mem::write<int32_t>(ambient + 4 * k, 0);
        call[2 + k] = ambient + 4 * k;
    }
    int32_t result = static_cast<int32_t>(call_guest(s, redirect_original(), std::span<const uint32_t>(call), false));
    int32_t current[3];
    if (result <= 52 - 3) {
        std::array<uint32_t, 51> retry{};
        retry[0] = args[0];
        retry[1] = fallback_format;
        for (unsigned k = 0; k < 49; ++k) {
            retry[2 + k] = args[5 + k];
        }
        call_guest(s, redirect_original(), std::span<const uint32_t>(retry), false);
        for (int k = 0; k < 3; ++k) {
            current[k] = last[k];
        }
        std::fprintf(stderr, "[SP] timecyc: line with %d of 52 values reparsed with the RAINY_COUNTRYSIDE format (%d so far)\n",
                     result, ++fallbacks);
    } else {
        for (int k = 0; k < 3; ++k) {
            current[k] = mem::read<int32_t>(ambient + 4 * static_cast<uint32_t>(k));
            last[k] = current[k];
        }
    }
    for (unsigned k = 0; k < 3; ++k) {
        mem::write<int32_t>(args[2 + k], current[k]);
    }
    eax(s) = static_cast<uint32_t>(result);
    return_to_caller(s);
    return m;
}

Memory* script_sprites_linear(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    static const uint32_t previous = allocator().alloc(4);
    constexpr uint32_t kTextureFilter = 9;
    constexpr uint32_t kFilterLinear = 2;
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    uint32_t set = mem::read<uint32_t>(engine + 0x20);
    uint32_t get = mem::read<uint32_t>(engine + 0x24);
    uint32_t arg = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, get, kTextureFilter, previous);
    guest_call<Conv::cdecl_>(s, set, kTextureFilter, kFilterLinear);
    guest_call<Conv::cdecl_>(s, redirect_original(), arg);
    guest_call<Conv::cdecl_>(s, set, kTextureFilter, mem::read<uint32_t>(previous));
    log_once(reached, "script sprites: drawn with bilinear filtering");
    return_to_caller(s);
    return m;
}

uint32_t next_rand31() {
    static std::mutex lock;
    static uint64_t seed = static_cast<uint64_t>(std::time(nullptr));
    std::lock_guard guard(lock);
    seed = 0x5851F42D4C957F2Dull * seed + 1;
    return static_cast<uint32_t>((seed >> 32) & 0x7fffffff);
}

Memory* rand31(GuestState& s, uint32_t, Memory* m) {
    eax(s) = next_rand31();
    return_to_caller(s);
    return m;
}

Memory* create_mirror_buffers(GuestState& s, uint32_t, Memory* m) {
    if (mem::read<uint32_t>(kMirrorBuffer) == 0) {
        uint32_t msaa0 = mem::read<uint32_t>(kMsaaValues);
        uint32_t msaa1 = mem::read<uint32_t>(kMsaaValues + 4);
        mem::write<uint32_t>(kMsaaValues, 0);
        mem::write<uint32_t>(kMsaaValues + 4, 0);
        int32_t quality = static_cast<int32_t>(mem::read<uint32_t>(kFxQuality));
        uint32_t width = quality >= 3 ? 2048 : quality >= 1 ? 1024 : 512;
        uint32_t height = width / 2;
        mem::write<uint32_t>(kMirrorBuffer, guest_call<Conv::cdecl_, uint32_t>(s, kRwRasterCreate, width, height, 0u, 5u));
        mem::write<uint32_t>(kMirrorZBuffer, guest_call<Conv::cdecl_, uint32_t>(s, kRwRasterCreate, width, height, 0u, 1u));
        mem::write<uint32_t>(kMsaaValues, msaa0);
        mem::write<uint32_t>(kMsaaValues + 4, msaa1);
        std::fprintf(stderr, "[SP] mirrors: %ux%u mirror buffer with its own z-buffer and no MSAA (visual FX quality %d)\n", width,
                     height, quality);
    }
    return_to_caller(s);
    return m;
}

uint32_t car_damage_component(GuestState& s, uint32_t) {
    float impulse = mem::read<float>(esp(s) + 0x2c);
    uint32_t edi_v = edi(s);
    eax(s) = mem::read<uint8_t>(edi_v + (0.0f < impulse ? 0x21 : 0x24));
    return 0x005428ef;
}

uint32_t weapon_range_vehicle_check(GuestState& s, uint32_t) {
    uint32_t ped = edx(s);
    bool in_vehicle = (mem::read<uint32_t>(ped + 0x46c) & 0x100) != 0;
    eax(s) = in_vehicle ? mem::read<uint32_t>(ped + 0x58c) : 0;
    return 0x0073b3b4;
}

uint32_t stencil_shadow_pads(GuestState&, uint32_t) {
    static const uint32_t pads = [] {
        uint32_t base = allocator().alloc(3 * 0x6000);
        std::fprintf(stderr, "[SP] stencil shadows: three 24 KB pads allocated once at 0x%08x\n", base);
        return base;
    }();
    for (uint32_t k = 0; k < 3; ++k) {
        mem::write<uint32_t>(kStencilShadowPads + 4 * k, pads + k * 0x6000);
    }
    return 0x00711e08;
}

constexpr uint32_t kPlayers = 0x00b7cd98;
constexpr uint32_t kPlayerInFocus = 0x00b7cd74;
constexpr uint32_t kFindPlayerPedFn = 0x0056e210;
constexpr uint32_t kIsPlayerOnAMission = 0x00464d50;
constexpr uint32_t kDrawCrossHair = 0x00a44490;
constexpr uint32_t kCompsUsed = 0x00b4e478;
constexpr uint32_t kCompsToUse = 0x008a6458;

float meters_to_feet(float) { return 3.280839895f; }

float light_status_randomness(float) { return 1.0f / 25000.0f; }

uint64_t audio_utility_time_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

Memory* set_position_and_area_code(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t entity = ecx(s);
    uint32_t x = stack_arg(s, 0);
    uint32_t y = stack_arg(s, 1);
    uint32_t zbits = stack_arg(s, 2);
    guest_call<Conv::thiscall>(s, redirect_original(), entity, x, y, zbits);
    if (std::bit_cast<float>(zbits) >= 950.0f) {
        mem::write<uint8_t>(entity + 0x2f, 13);
        log_once(reached, "car generator: vehicle above z 950 placed in interior area 13");
    }
    return_to_caller(s, 12);
    return m;
}

uint32_t player_info(int32_t id) {
    uint32_t index = id < 0 ? mem::read<uint8_t>(kPlayerInFocus) : static_cast<uint32_t>(id);
    return kPlayers + index * 0x190;
}

uint32_t ped_current_vehicle(uint32_t ped) {
    if (ped && (mem::read<uint32_t>(ped + 0x46c) & 0x100)) {
        return mem::read<uint32_t>(ped + 0x58c);
    }
    return 0;
}

uint32_t find_player_entity_with_rc(int32_t id) {
    uint32_t info = player_info(id);
    uint32_t ped = mem::read<uint32_t>(info);
    if (uint32_t remote = mem::read<uint32_t>(info + 0xb0)) {
        return remote;
    }
    if (uint32_t vehicle = ped_current_vehicle(ped)) {
        return vehicle;
    }
    return ped;
}

Memory* find_player_vehicle_with_rc(GuestState& s, uint32_t, Memory* m) {
    mem::write<uint32_t>(esp(s) + 8, 1);
    eip(s) = redirect_original();
    return m;
}

Memory* give_weapon_min_ammo(GuestState& s, uint32_t, Memory* m) {
    uint32_t ammo = esp(s) + 8;
    if (mem::read<uint32_t>(ammo) == 0) {
        mem::write<uint32_t>(ammo, 1);
        static std::atomic<bool> reached = false;
        log_once(reached, "script: GIVE_WEAPON_TO_CHAR with 0 ammo gives 1");
    }
    eip(s) = redirect_original();
    return m;
}

uint32_t bicycle_fire_driver(GuestState& s, uint32_t) {
    uint32_t vehicle = edi(s);
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), vehicle);
    eax(s) = mem::read<uint32_t>(vehicle + 0x460);
    return 0x0053a9ae;
}

Memory* bicycle_fire_noop(GuestState& s, uint32_t, Memory* m) {
    return_to_caller(s);
    return m;
}

Memory* start_fire_null_check(GuestState& s, uint32_t, Memory* m) {
    if (stack_arg(s, 0) == 0) {
        static std::atomic<bool> reached = false;
        log_once(reached, "fire: bicycle without a driver starts no fire on CJ");
        return_to_caller(s, 24);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* corona_rotate_aspect(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    log_once(reached, "coronas: rotation argument 20 x recipz");
    float recipz = std::bit_cast<float>(stack_arg(s, 9));
    mem::write<float>(esp(s) + 4 + 4 * 10, 20.0f * recipz);
    eip(s) = redirect_original();
    return m;
}

Memory* render_shadows_alpha_always(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t previous = allocator().alloc(4);
    static std::atomic<bool> reached = false;
    log_once(reached, "shadows: static and stored shadows drawn with the alpha test set to always");
    constexpr uint32_t kAlphaTestFunction = 0x1d;
    constexpr uint32_t kAlphaAlways = 8;
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    uint32_t set = mem::read<uint32_t>(engine + 0x20);
    uint32_t get = mem::read<uint32_t>(engine + 0x24);
    guest_call<Conv::cdecl_>(s, get, kAlphaTestFunction, previous);
    guest_call<Conv::cdecl_>(s, set, kAlphaTestFunction, kAlphaAlways);
    guest_call<Conv::cdecl_>(s, redirect_original());
    guest_call<Conv::cdecl_>(s, set, kAlphaTestFunction, mem::read<uint32_t>(previous));
    return_to_caller(s);
    return m;
}

uint32_t reset_comps_for_no_extras(GuestState&, uint32_t) {
    mem::write<int8_t>(kCompsUsed, -1);
    mem::write<int8_t>(kCompsUsed + 1, -1);
    mem::write<int8_t>(kCompsToUse, -2);
    mem::write<int8_t>(kCompsToUse + 1, -2);
    return 0x004c97bf;
}

uint32_t follow_car_mouse_forward(GuestState& s, uint32_t) {
    bool use = mem::read<uint8_t>(0x00b6ec2e) != 0 && mem::read<uint32_t>(0x008cc384) == 3;
    eax(s) = (eax(s) & ~0xffu) | (use ? 1u : 0u);
    return 0x0052561a;
}

uint32_t open_top_or_quad(GuestState& s, uint32_t) {
    uint32_t vehicle = ecx(s);
    uint32_t vtable = mem::read<uint32_t>(vehicle);
    bool open = guest_call<Conv::thiscall, uint8_t>(s, mem::read<uint32_t>(vtable + 0x9c), vehicle) != 0;
    bool quad = mem::read<uint32_t>(vehicle + 0x594) == 2;
    eax(s) = (eax(s) & ~0xffu) | ((open || quad) ? 1u : 0u);
    return 0x005e69c4;
}

uint32_t quad_handlebar_interp(GuestState& s, uint32_t) {
    uint32_t sp = esp(s);
    bool reverse = mem::read<uint32_t>(sp + 0x2c) != 0;
    bool driveby = mem::read<uint32_t>(sp + 0x34) != 0;
    if (reverse || driveby || std::fabs(mem::read<float>(sp + 0x24)) < 0.02f) {
        mem::write<uint32_t>(sp + 0x14, 0);
    }
    x87_push(s, 0.86f);
    return 0x006b7938;
}

Memory* weapon_info_or_camera(GuestState& s, uint32_t, Memory* m) {
    if (mem::read<uint32_t>(kDrawCrossHair) == 2) {
        mem::write<uint32_t>(esp(s) + 4, 43);
    }
    eip(s) = redirect_original();
    return m;
}

uint32_t biker_cop_model(GuestState& s, uint32_t pc) {
    if (ecx(s) == 4 && mem::read<uint32_t>(esp(s) + 4) == 6) {
        mem::write<uint32_t>(eax(s), 1);
        static std::atomic<bool> reached = false;
        log_once(reached, "script: PEDTYPE_COP with lapdm1 spawns a biker cop");
        return 0x00464fb5;
    }
    return pc;
}

uint32_t remove_driver_status(GuestState& s, uint32_t) {
    uint8_t status = mem::read<uint8_t>(edi(s) + 0x36);
    uint8_t al = status;
    if ((status & 0xf8) != 0x28) {
        al = static_cast<uint8_t>((status & 7) | 0x20);
    }
    eax(s) = (eax(s) & ~0xffu) | al;
    return 0x006d195c;
}

Memory* prepare_vehicle_for_ped_exit(GuestState& s, uint32_t, Memory* m) {
    uint32_t vehicle = mem::read<uint32_t>(ecx(s) + 0x0c);
    if (vehicle && (mem::read<uint8_t>(vehicle + 0x36) >> 3) == 5) {
        static std::atomic<bool> reached = false;
        log_once(reached, "vehicle: a wrecked vehicle is not set up again for the driver's exit");
        return_to_caller(s, 4);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* rand16(GuestState& s, uint32_t, Memory* m) {
    uint32_t low = guest_call<Conv::cdecl_, uint32_t>(s, redirect_original());
    uint32_t high = guest_call<Conv::cdecl_, uint32_t>(s, redirect_original()) & 1;
    static std::atomic<bool> reached = false;
    log_once(reached, "script: random numbers are 16-bit");
    eax(s) = low | (high << 15);
    return_to_caller(s);
    return m;
}

Memory* shooting_stars_untextured(GuestState& s, uint32_t, Memory* m) {
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    std::array<uint32_t, 4> args{};
    for (unsigned i = 0; i < args.size(); ++i) {
        args[i] = stack_arg(s, i);
    }
    guest_call<Conv::cdecl_>(s, mem::read<uint32_t>(engine + 0x20), 1u, 0u);
    eax(s) = call_guest(s, redirect_original(), std::span<const uint32_t>(args), false);
    return_to_caller(s);
    return m;
}

Memory* plane_sector_numbers(GuestState& s, uint32_t, Memory* m) {
    float mul_x = mem::read<float>(mem::read<uint32_t>(0x0041acef));
    float add_x = mem::read<float>(mem::read<uint32_t>(0x0041acf8));
    float mul_y = mem::read<float>(mem::read<uint32_t>(0x0041ad3e));
    float add_y = mem::read<float>(mem::read<uint32_t>(0x0041ad44));
    int32_t x = static_cast<int32_t>(stack_arg(s, 0));
    int32_t y = static_cast<int32_t>(stack_arg(s, 1));
    int32_t sx = static_cast<int32_t>(static_cast<float>(x) * mul_x + add_x);
    int32_t sy = static_cast<int32_t>(static_cast<float>(y) * mul_y + add_y);
    mem::write<int32_t>(esp(s) + 4, sx);
    mem::write<int32_t>(esp(s) + 8, sy);
    static std::atomic<bool> reached = false;
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] planes: spawn check at %d,%d uses sector %d,%d\n", x, y, sx, sy);
    }
    eip(s) = redirect_original();
    return m;
}

uint32_t riot_skip_player_group(GuestState& s, uint32_t pc) {
    if (mem::read<uint8_t>(ebp(s) + 0x2d0) != 1) {
        return pc;
    }
    SavedScratch saved(s);
    if (guest_call<Conv::cdecl_, uint8_t>(s, kIsPlayerOnAMission) == 0) {
        return pc;
    }
    static std::atomic<bool> reached = false;
    log_once(reached, "riot: the player's group is not targeted during a mission");
    return 0x006cd7f4;
}

constexpr uint32_t kRwInitialised = 0x00c920e8;
constexpr uint32_t kPrefsFrameLimiter = 0x00ba6794;
constexpr uint32_t kForceFrameLimit = 0x00a43088;
constexpr uint32_t kAudioEngine = 0x00b6bc90;
constexpr uint32_t kAudioEngineFrameLimitCheck = 0x005071d0;
constexpr uint32_t kRsFrameLimit = 0x00c1704c;
constexpr uint32_t kRenderFrame = 0x00748d98;
constexpr uint32_t kSkipFrame = 0x00748da3;

int configured_frame_limit() {
    auto choice = mod_settings().choice("silentpatch.frame_limit");
    if (choice == "60") {
        return 60;
    }
    if (choice == "uncapped") {
        return 0;
    }
    return 30;
}

uint32_t frame_pacing(GuestState& s, uint32_t) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next_frame{};
    static std::atomic<bool> logged = false;
    if (!mem::read<uint32_t>(kRwInitialised)) {
        return kSkipFrame;
    }
    bool prefs_limit = mem::read<uint8_t>(kPrefsFrameLimiter) != 0;
    bool forced = mem::read<uint8_t>(kForceFrameLimit) == 1;
    bool limited = prefs_limit || forced;
    if (!limited) {
        uint32_t saved_ecx = ecx(s);
        limited = guest_call<Conv::thiscall, uint8_t>(s, kAudioEngineFrameLimitCheck, kAudioEngine) != 0;
        ecx(s) = saved_ecx;
    }
    int fps = 0;
    if (limited) {
        fps = configured_frame_limit();
        if (fps == 0 && !prefs_limit) {
            fps = static_cast<int>(mem::read<uint32_t>(kRsFrameLimit));
        }
    }
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[SP] frame limiter: %d fps (silentpatch.frame_limit = %s, menu limiter %s)\n", fps,
                     mod_settings().choice("silentpatch.frame_limit").c_str(), prefs_limit ? "on" : "off");
    }
    auto now = clock::now();
    if (fps > 0) {
        auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
        if (next_frame.time_since_epoch().count() == 0 || now - next_frame > period * 2) {
            next_frame = now;
        }
        if (next_frame > now + std::chrono::milliseconds(2)) {
            std::this_thread::sleep_until(next_frame - std::chrono::milliseconds(1));
        }
        while (clock::now() < next_frame) {
        }
        next_frame += period;
    } else {
        next_frame = now;
    }
    return kRenderFrame;
}

std::mutex& cdstream_lock() {
    static std::mutex lock;
    return lock;
}

uint32_t cdstream_sync(GuestState& s, uint32_t) {
    static std::atomic<bool> waited = false;
    uint32_t stream = esi(s);
    SavedRegs saved(s);
    std::unique_lock guard(cdstream_lock());
    if (mem::read<uint32_t>(stream + 0x04) != 0) {
        mem::write<uint8_t>(stream + 0x0d, 1);
        guard.unlock();
        log_once(waited, "cdstream: sync waits under the channel lock");
        guest_call<Conv::stdcall, uint32_t>(s, mem::read<uint32_t>(kWaitForSingleObjectIat), mem::read<uint32_t>(stream + 0x14),
                                            0xffffffffu);
        guard.lock();
    }
    mem::write<uint8_t>(stream + 0x0e, 0);
    eax(s) = mem::read<uint32_t>(stream + 0x10);
    return 0x0156cdbb;
}

uint32_t cdstream_thread_done(GuestState& s, uint32_t) {
    uint32_t stream = esi(s);
    SavedRegs saved(s);
    std::lock_guard guard(cdstream_lock());
    mem::write<uint32_t>(stream + 0x04, 0);
    if (mem::read<uint8_t>(stream + 0x0d) != 0) {
        guest_call<Conv::stdcall, uint32_t>(s, mem::read<uint32_t>(kReleaseSemaphoreIat), mem::read<uint32_t>(stream + 0x14), 1u,
                                            0u);
    }
    mem::write<uint8_t>(stream + 0x0e, 0);
    return 0x00406681;
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

constexpr uint32_t kSetupBackBufferVertex = 0x007043d0;
constexpr uint32_t kHeatHazeFxTypeLast = 0x008d50e4;
constexpr uint32_t kGetFrameNodeName = 0x0072fb30;
constexpr uint32_t kClearAtomicFlag = 0x00732310;
constexpr uint32_t kIsInPlayersGroup = 0x005f7f10;
constexpr uint32_t kMenuScaleWidthOperand = 0x005733ff;
constexpr uint32_t kMenuScaleHeightOperand = 0x0057342f;
constexpr uint32_t kFontScaleWidthOperand = 0x00719c0f;
constexpr uint32_t kFontScaleHeightOperand = 0x00719c29;
constexpr uint32_t kReplayScaleWidthOperand = 0x0045c257;
constexpr uint32_t kAtomicIsLeft = 4;
constexpr double kCreditsHeight = 448.0;
constexpr double kBigMessageSlideEnd = 620.0;

float screen_width() { return static_cast<float>(static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumWidth))); }

float screen_height() { return static_cast<float>(static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumHeight))); }

float operand_value(uint32_t operand) { return mem::read<float>(mem::read<uint32_t>(operand)); }

float menu_width() { return screen_width() * operand_value(kMenuScaleWidthOperand); }

float menu_height() { return screen_height() * operand_value(kMenuScaleHeightOperand); }

float font_width() { return screen_width() * operand_value(kFontScaleWidthOperand); }

float font_height() { return screen_height() * operand_value(kFontScaleHeightOperand); }

float replay_width() { return screen_width() * operand_value(kReplayScaleWidthOperand); }

uint32_t float_arg(GuestState& s, unsigned index) { return esp(s) + 4 + 4 * index; }

void scale_float_arg(GuestState& s, unsigned index, float k) {
    uint32_t addr = float_arg(s, index);
    mem::write<float>(addr, mem::read<float>(addr) * k);
}

uint32_t credits_fixed_height(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    log_once(reached, "credits: lines are placed on a fixed 448-unit screen height");
    x87_push(s, kCreditsHeight);
    return 0x005a8666;
}

uint32_t credits_end_fixed_height(GuestState& s, uint32_t) {
    ecx(s) = static_cast<uint32_t>(kCreditsHeight);
    return 0x005af8cf;
}

float credits_top_margin(float) { return 1.0f; }

float credits_bottom_margin(float initial) { return -initial; }

Memory* credits_set_scale(GuestState& s, uint32_t, Memory* m) {
    scale_float_arg(s, 0, stuff2d_width());
    scale_float_arg(s, 1, stuff2d_height());
    eip(s) = redirect_original();
    return m;
}

Memory* credits_print(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    scale_float_arg(s, 1, stuff2d_height());
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] credits: text scaled by %.3f x %.3f\n", stuff2d_width(), stuff2d_height());
    }
    eip(s) = redirect_original();
    return m;
}

Memory* credits_print_shadow(GuestState& s, uint32_t, Memory* m) {
    uint32_t x = float_arg(s, 0);
    mem::write<float>(x, mem::read<float>(x) + 1.0f - stuff2d_width());
    scale_float_arg(s, 1, stuff2d_height());
    eip(s) = redirect_original();
    return m;
}

uint32_t big_message_fixed_width(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    log_once(reached, "big message: the slide-out threshold is 620 of a 640-unit screen width");
    x87_push(s, kBigMessageSlideEnd);
    return 0x0058c916;
}

uint32_t video_mode_post_fx(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    mem::write<int32_t>(kHeatHazeFxTypeLast, -1);
    {
        SavedScratch saved(s);
        guest_call<Conv::cdecl_>(s, kSetupBackBufferVertex);
    }
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] post effects: back buffer vertices set up after the mode change (%dx%d), heat haze reinitialises\n",
                     static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumWidth)),
                     static_cast<int32_t>(mem::read<uint32_t>(kRsMaximumHeight)));
    }
    return pc;
}

Memory* underwater_ripple_frequency(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    scale_float_arg(s, 5, 480.0f / screen_height());
    log_once(reached, "post effects: underwater ripple frequency scaled to 480 lines");
    eip(s) = redirect_original();
    return m;
}

Memory* crosshair_sprite_scale(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    scale_float_arg(s, 3, stuff2d_width());
    scale_float_arg(s, 4, stuff2d_height());
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] crosshairs: sprite at 0x%08x scaled by %.3f x %.3f\n", redirect_site(), stuff2d_width(),
                     stuff2d_height());
    }
    eip(s) = redirect_original();
    return m;
}

float crosshair_triangle(float initial) { return initial * stuff2d_height(); }

float map_cursor_x(float initial) { return initial * menu_width(); }

float map_cursor_y(float initial) { return initial * menu_height(); }

Memory* map_limit_scaled(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t x = stack_arg(s, 0);
    uint32_t y = stack_arg(s, 1);
    float sx = menu_width();
    float sy = menu_height();
    mem::write<float>(x, mem::read<float>(x) * sx);
    mem::write<float>(y, mem::read<float>(y) * sy);
    guest_call<Conv::cdecl_>(s, redirect_original(), x, y);
    mem::write<float>(x, mem::read<float>(x) / sx);
    mem::write<float>(y, mem::read<float>(y) / sy);
    log_once(reached, "map: LimitToMap gets scaled coordinates and returns unscaled ones");
    return_to_caller(s);
    return m;
}

float text_padding_x(float initial) { return initial * font_width(); }

float text_padding_y(float initial) { return initial * font_height(); }

void wrap_right(GuestState& s, float scale) {
    uint32_t addr = float_arg(s, 0);
    float origin = screen_width();
    mem::write<float>(addr, (mem::read<float>(addr) - origin) * scale + origin);
}

Memory* wrap_full_width(GuestState& s, uint32_t, Memory* m) {
    mem::write<float>(float_arg(s, 0), screen_width());
    eip(s) = redirect_original();
    return m;
}

Memory* wrap_menu_left(GuestState& s, uint32_t, Memory* m) {
    scale_float_arg(s, 0, menu_width());
    eip(s) = redirect_original();
    return m;
}

Memory* wrap_menu_right(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    wrap_right(s, menu_width());
    log_once(reached, "line wraps: menu text wraps scale to resolution");
    eip(s) = redirect_original();
    return m;
}

Memory* wrap_replay_right(GuestState& s, uint32_t, Memory* m) {
    wrap_right(s, replay_width());
    eip(s) = redirect_original();
    return m;
}

uint32_t nitrous_reverse(GuestState& s, uint32_t) {
    float gas = mem::read<float>(esi(s) + 0x49c);
    if (gas < 0.0f) {
        static std::atomic<bool> reversing = false;
        log_once(reversing, "nitrous: no faster recharge while reversing");
    } else {
        x87_set_st0(s, x87_st0(s) - gas);
    }
    return 0x006a4081;
}

Memory* jet_matrix_flags(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t result =
        guest_call<Conv::cdecl_, uint32_t>(s, redirect_original(), stack_arg(s, 0), stack_arg(s, 1), stack_arg(s, 2));
    if (result) {
        mem::write<uint32_t>(result + 0x0c, 0);
    }
    log_once(reached, "jet thrusters: the multiplied matrix's flags are cleared");
    eax(s) = result;
    return_to_caller(s);
    return m;
}

int32_t rustler_anim_block(GuestState& s) {
    static std::optional<int32_t> index;
    if (!index) {
        index = guest_call<Conv::cdecl_, int32_t>(s, kGetAnimationBlockIndex, guest_literal("rustler"));
    }
    return *index;
}

Memory* hunter_door_unmark(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t info = ecx(s);
    guest_call<Conv::thiscall>(s, redirect_original(), info);
    uint32_t vtable = mem::read<uint32_t>(info);
    auto anim = guest_call<Conv::thiscall, int32_t>(s, mem::read<uint32_t>(vtable + 0x38), info);
    uint32_t clump = mem::read<uint32_t>(info + 0x1c);
    if (anim != -1 && clump && anim == rustler_anim_block(s)) {
        uint32_t head = clump + 0x08;
        for (uint32_t link = mem::read<uint32_t>(head); link != head; link = mem::read<uint32_t>(link)) {
            uint32_t atomic = link - 0x40;
            uint32_t name = guest_call<Conv::cdecl_, uint32_t>(s, kGetFrameNodeName, mem::read<uint32_t>(atomic + 0x04));
            if (name && mem::read_cstr(name, 8).starts_with("door_lf")) {
                guest_call<Conv::cdecl_>(s, kClearAtomicFlag, atomic, kAtomicIsLeft);
                if (!reached.exchange(true)) {
                    std::fprintf(stderr, "[SP] hunter door: door_lf of model info 0x%08x is no longer marked as a left atomic\n", info);
                }
            }
        }
    }
    return_to_caller(s);
    return m;
}

uint32_t gang_war_friendly_blips(GuestState& s, uint32_t) {
    uint32_t ped = esi(s);
    uint32_t flags = mem::read<uint32_t>(ped + 0x474);
    if (flags & 0x2000) {
        bool in_group = false;
        {
            SavedScratch saved(s);
            in_group = guest_call<Conv::cdecl_, uint8_t>(s, kIsInPlayersGroup, ped) != 0;
        }
        if (in_group) {
            static std::atomic<bool> kept = false;
            log_once(kept, "gang war: a player's group member keeps its blip");
            flags &= ~0x2000u;
        }
    }
    eax(s) = flags;
    return 0x01560b61;
}

constexpr uint32_t kPedSay = 0x005effe0;
constexpr uint32_t kConversationTopic = 0x009691bc;
constexpr uint32_t kSetCJMood = 0x004e3ed0;
constexpr uint32_t kFindPlayerWanted = 0x0056e230;
constexpr uint32_t kGetTaskUseGun = 0x00600f70;
constexpr uint32_t kSetCentreSize = 0x007194e0;
constexpr uint32_t kSpeechContextLookup = 0x008c6a68;
constexpr uint32_t kGenSpeechLookup = 0x008af8a8;
constexpr uint32_t kEmgSpeechLookup = 0x008ba4d0;
constexpr uint32_t kGfdSpeechLookup = 0x008c42b0;
constexpr uint32_t kPlySpeechLookup = 0x008bbef8;
constexpr uint32_t kGngSpeechLookup = 0x008be620;
constexpr uint32_t kSpecialPedVoiceLookup = 0x008c6948;
constexpr uint32_t kSpecialPedVoiceNameLookup = 0x008c65c0;
constexpr uint32_t kNumSpecialPedVoices = 45;
constexpr uint32_t kPedFlags = 0x46c;
constexpr uint32_t kPedIntelligence = 0x47c;
constexpr uint32_t kPedState = 0x530;
constexpr uint32_t kPedType = 0x598;
constexpr uint32_t kPedVehicle = 0x58c;
constexpr uint32_t kMouseLockOnRecruitPed = 0x79c;
constexpr uint32_t kBeingChasedByPolice = 0x00800000;
constexpr uint16_t kContextAbuseGangLsv = 2;
constexpr uint16_t kContextAttackPlayer = 20;
constexpr uint16_t kContextChased = 44;
constexpr uint16_t kContextCrashGeneric = 68;
constexpr uint16_t kContextGoodCitizen = 113;
constexpr uint16_t kContextLikeDismissFemale = 131;
constexpr uint16_t kContextLikeDismissMale = 132;
constexpr uint16_t kContextWeatherDislReply = 233;
constexpr uint16_t kContextWeatherLikeReply = 234;
constexpr uint16_t kContextWhereYouFromPosReply = 253;
constexpr uint16_t kContextPainSprayed = 348;
constexpr uint32_t kConvWeather = 7;
constexpr uint32_t kConvWhereYouFrom = 8;
constexpr uint32_t kConvGangbang = 9;

int32_t ped_type(uint32_t ped) { return mem::read<int32_t>(ped + kPedType); }

bool is_gang_ped(uint32_t ped) { return ped_type(ped) >= 7 && ped_type(ped) <= 16; }

bool is_ped_entity(uint32_t entity) { return (mem::read<uint8_t>(entity + 0x36) & 7) == 3; }

int16_t ped_say(GuestState& s, uint32_t ped, uint16_t phrase, uint32_t delay = 0, float probability = 1.0f, uint32_t silence = 0,
                uint32_t audible = 0, uint32_t frontend = 0) {
    return guest_call<Conv::thiscall, int16_t>(s, kPedSay, ped, static_cast<uint32_t>(phrase), delay, probability, silence, audible,
                                               frontend);
}

int16_t say_original(GuestState& s, uint32_t ped, uint16_t phrase) {
    return guest_call<Conv::thiscall, int16_t>(s, redirect_original(), ped, static_cast<uint32_t>(phrase), stack_arg(s, 1),
                                               std::bit_cast<float>(stack_arg(s, 2)), stack_arg(s, 3), stack_arg(s, 4),
                                               stack_arg(s, 5));
}

void return_say(GuestState& s, int16_t result) {
    eax(s) = static_cast<uint32_t>(static_cast<int32_t>(result));
    return_to_caller(s, 24);
}

void set_specific_speech_context(int16_t global, int16_t ped_audio_type, int16_t specific) {
    for (uint32_t row = kSpeechContextLookup; mem::read<int16_t>(row) != -1; row += 16) {
        if (mem::read<int16_t>(row) == global) {
            uint32_t slot = row + 2 * static_cast<uint32_t>(ped_audio_type + 1);
            if (mem::read<int16_t>(slot) == -1) {
                mem::write<int16_t>(slot, specific);
            }
            return;
        }
    }
}

template <typename T>
void move_speech_context(uint32_t table, uint32_t voices, uint32_t voice, uint32_t source, uint32_t destination) {
    constexpr T kNoVoice = static_cast<T>(-1);
    uint32_t src = table + (source * voices + voice) * 2 * sizeof(T);
    uint32_t dst = table + (destination * voices + voice) * 2 * sizeof(T);
    if (mem::read<T>(src) != kNoVoice && mem::read<T>(src + sizeof(T)) != kNoVoice && mem::read<T>(dst) == kNoVoice &&
        mem::read<T>(dst + sizeof(T)) == kNoVoice) {
        mem::write<T>(dst, mem::read<T>(src));
        mem::write<T>(dst + sizeof(T), mem::read<T>(src + sizeof(T)));
        mem::write<T>(src, kNoVoice);
        mem::write<T>(src + sizeof(T), kNoVoice);
    }
}

uint32_t speech_lookup_tables(GuestState&, uint32_t pc) {
    static constexpr int16_t kSpecific[][3] = {
        {208, 2, 102}, {209, 2, 104}, {210, 2, 96},  {211, 2, 97},  {212, 2, 101}, {213, 2, 99},  {214, 2, 100},
        {215, 2, 98},  {216, 2, 103}, {217, 2, 107}, {218, 2, 105}, {219, 2, 106}, {56, 4, 9},    {114, 4, 64},
        {133, 4, 67},  {161, 4, 68},  {162, 4, 69},  {163, 4, 70},  {164, 4, 71},  {165, 4, 72},  {166, 4, 73},
        {167, 4, 74},  {168, 4, 75},  {169, 4, 76},  {170, 4, 77},  {171, 4, 78},  {179, 4, 80},  {181, 4, 82},
        {227, 4, 83},  {115, 4, 107}, {184, 0, 93},  {184, 4, 86},  {5, 3, 67},    {7, 3, 66},    {8, 3, 109},
    };
    for (const auto& c : kSpecific) {
        set_specific_speech_context(c[0], c[1], c[2]);
    }
    for (uint32_t voice = 0; voice < 209; ++voice) {
        move_speech_context<uint8_t>(kGenSpeechLookup, 209, voice, 98, 22);
        move_speech_context<uint8_t>(kGenSpeechLookup, 209, voice, 99, 49);
        move_speech_context<uint8_t>(kGenSpeechLookup, 209, voice, 100, 18);
        move_speech_context<uint8_t>(kGenSpeechLookup, 209, voice, 101, 19);
    }
    move_speech_context<uint8_t>(kGenSpeechLookup, 209, 153, 92, 16);
    for (uint32_t voice = 0; voice < 46; ++voice) {
        move_speech_context<int16_t>(kEmgSpeechLookup, 46, voice, 32, 23);
    }
    for (uint32_t voice = 0; voice < 18; ++voice) {
        move_speech_context<int16_t>(kGfdSpeechLookup, 18, voice, 86, 117);
        move_speech_context<int16_t>(kGfdSpeechLookup, 18, voice, 110, 3);
    }
    for (uint32_t voice = 0; voice < 20; ++voice) {
        move_speech_context<int16_t>(kPlySpeechLookup, 20, voice, 11, 26);
    }
    for (uint32_t voice = 0; voice < 52; ++voice) {
        move_speech_context<int16_t>(kGngSpeechLookup, 52, voice, 105, 52);
    }
    for (uint32_t i = 0; i < kNumSpecialPedVoices; ++i) {
        uint32_t voice = kSpecialPedVoiceLookup + 6 * i;
        if (mem::read<int16_t>(voice) == -1 && mem::read<int16_t>(voice + 2) == -1) {
            uint32_t name = kSpecialPedVoiceNameLookup + 20 * i;
            for (uint32_t b = 0; b < 20; ++b) {
                mem::write<uint8_t>(name + b, 0);
            }
            mem::write_cstr(name, "maccer");
            mem::write<int16_t>(voice, 23);
            mem::write<int16_t>(voice + 2, 3);
            mem::write<int16_t>(voice + 4, 0);
            std::fprintf(stderr, "[SP] speech: Maccer gets the special voice slot %u\n", i);
            break;
        }
    }
    std::fprintf(stderr, "[SP] speech: lookup tables patched (turf cheers, girlfriend and shopkeeper lines, gang taunts, typo'd contexts)\n");
    return pc;
}

Memory* say_weather_reply(GuestState& s, uint32_t, Memory* m) {
    auto phrase = static_cast<uint16_t>(stack_arg(s, 0));
    if ((phrase == kContextLikeDismissFemale || phrase == kContextLikeDismissMale) &&
        mem::read<uint32_t>(kConversationTopic) == kConvWeather) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: a positive reply to a weather complaint uses WEATHER_DISL_REPLY");
        mem::write<uint32_t>(float_arg(s, 0), kContextWeatherDislReply);
    }
    eip(s) = redirect_original();
    return m;
}

bool g_force_cd_mood = false;

Memory* sound_bank_weather_fallback(GuestState& s, uint32_t, Memory* m) {
    uint32_t entity = ecx(s);
    uint32_t context = stack_arg(s, 0);
    uint32_t extra = stack_arg(s, 1);
    auto result = guest_call<Conv::thiscall, int16_t>(s, redirect_original(), entity, context, extra);
    auto global = static_cast<int16_t>(context);
    if (result < 0 && mem::read<int16_t>(entity + 0x92) == 2 &&
        (global == kContextWeatherDislReply || global == kContextWeatherLikeReply)) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: CJ's weather reply falls back to the CD mood");
        g_force_cd_mood = true;
        result = guest_call<Conv::thiscall, int16_t>(s, redirect_original(), entity, context, extra);
        g_force_cd_mood = false;
    }
    eax(s) = static_cast<uint32_t>(static_cast<int32_t>(result));
    return_to_caller(s, 8);
    return m;
}

Memory* cj_mood_override(GuestState& s, uint32_t, Memory* m) {
    if (g_force_cd_mood) {
        eax(s) = 2;
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* get_voice_typos(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t kBympi = guest_literal("VOICE_GEN_BYMPI");
    static const uint32_t kWmysgrd = guest_literal("VOICE_GEN_WMYSGRD");
    uint32_t name = stack_arg(s, 0);
    uint32_t type = stack_arg(s, 1);
    auto result = guest_call<Conv::cdecl_, int16_t>(s, redirect_original(), name, type);
    if (result < 0) {
        std::string text = mem::read_cstr(name, 64);
        uint32_t alias = text == "VOICE_GEN_BMYPI" ? kBympi : text == "VOICE_GEN_WMYSGRAD" ? kWmysgrd : 0;
        if (alias) {
            result = guest_call<Conv::cdecl_, int16_t>(s, redirect_original(), alias, type);
            std::fprintf(stderr, "[SP] speech: %s found as %s (%d)\n", text.c_str(), mem::read_cstr(alias, 32).c_str(), result);
        }
    }
    eax(s) = static_cast<uint32_t>(static_cast<int32_t>(result));
    return_to_caller(s);
    return m;
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

Memory* peds_ide_voice_names(GuestState& s, uint32_t, Memory* m) {
    std::array<uint32_t, 16> a{};
    for (unsigned i = 0; i < a.size(); ++i) {
        a[i] = stack_arg(s, i);
    }
    auto result = guest_call<Conv::cdecl_, int32_t>(s, redirect_original(), a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
                                                    a[10], a[11], a[12], a[13], a[14], a[15]);
    if (result == 14) {
        std::string model = mem::read_cstr(a[3], 32);
        std::string voice1 = mem::read_cstr(a[14], 60);
        std::string voice2 = mem::read_cstr(a[15], 60);
        for (std::string_view prefix : {"sbfyst", "swfyst"}) {
            std::string name = std::string(prefix) + "r";
            std::string short_voice = "VOICE_GEN_" + std::string(prefix);
            for (auto& c : short_voice) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (equals_ignore_case(model, name) && voice1 == short_voice && voice2 == short_voice) {
                mem::write_cstr(a[14], short_voice + "R");
                mem::write_cstr(a[15], short_voice + "R");
                std::fprintf(stderr, "[SP] speech: %s uses %sR\n", model.c_str(), short_voice.c_str());
            }
        }
    }
    eax(s) = static_cast<uint32_t>(result);
    return_to_caller(s);
    return m;
}

Memory* turf_text_label_stricmp(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    log_once(reached, "speech: turf takeover cheers compare the text label");
    mem::write<uint32_t>(float_arg(s, 0), stack_arg(s, 0) + 8);
    eip(s) = redirect_original();
    return m;
}

std::array<uint32_t, 5> g_cj_mood_args{};

Memory* cj_mood_store(GuestState& s, uint32_t, Memory* m) {
    for (unsigned i = 0; i < g_cj_mood_args.size(); ++i) {
        g_cj_mood_args[i] = stack_arg(s, i);
    }
    return_to_caller(s);
    return m;
}

Memory* say_solicit_then_mood(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    int16_t result = say_original(s, ecx(s), static_cast<uint16_t>(stack_arg(s, 0)));
    auto& a = g_cj_mood_args;
    guest_call<Conv::cdecl_>(s, kSetCJMood, a[0], a[1], a[2], a[3], a[4]);
    log_once(reached, "speech: CJ's mood changes to WR after the solicit line");
    return_say(s, result);
    return m;
}

Memory* gang_abuse_lsv(GuestState& s, uint32_t, Memory* m) {
    uint32_t ped1 = stack_arg(s, 0);
    uint32_t ped2 = stack_arg(s, 1);
    if (ped1 && ped2 && is_gang_ped(ped1) && ped_type(ped2) == 9) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: a gang member taunts the Vagos");
        ped_say(s, ped1, kContextAbuseGangLsv);
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* gang_attack_player(GuestState& s, uint32_t, Memory* m) {
    uint32_t ped1 = stack_arg(s, 0);
    uint32_t ped2 = stack_arg(s, 1);
    if (ped1 && ped2 && is_gang_ped(ped1) && (ped_type(ped2) == 0 || ped_type(ped2) == 1)) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: a gang member taunts CJ when attacking him");
        ped_say(s, ped1, kContextAttackPlayer);
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* say_crash_generic_fallback(GuestState& s, uint32_t, Memory* m) {
    uint32_t ped = ecx(s);
    auto phrase = static_cast<uint16_t>(stack_arg(s, 0));
    int16_t result = say_original(s, ped, phrase);
    if (result < 0 && phrase != kContextCrashGeneric) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: a crash line falls back to CRASH_GENERIC");
        result = say_original(s, ped, kContextCrashGeneric);
    }
    return_say(s, result);
    return m;
}

uint32_t pain_sprayed(GuestState& s, uint32_t pc) {
    uint32_t ped = esi(s);
    uint32_t damage = ebp(s);
    uint32_t weapon = mem::read<uint32_t>(damage + 0x0c);
    if (weapon != 17 && weapon != 41 && weapon != 42) {
        return pc;
    }
    SavedScratch saved(s);
    uint32_t inflictor = mem::read<uint32_t>(damage);
    if (inflictor && is_ped_entity(inflictor)) {
        uint32_t task = guest_call<Conv::thiscall, uint32_t>(s, kGetTaskUseGun, mem::read<uint32_t>(inflictor + kPedIntelligence));
        if (task && mem::read<int8_t>(task + 0x10) == 5) {
            return pc;
        }
    }
    static std::atomic<bool> reached = false;
    log_once(reached, "speech: a sprayed ped says PAIN_SPRAYED");
    ped_say(s, ped, kContextPainSprayed);
    return 0x004b3342;
}

uint32_t ped_vehicle(uint32_t ped) {
    return (mem::read<uint32_t>(ped + kPedFlags) & 0x100) ? mem::read<uint32_t>(ped + kPedVehicle) : 0;
}

Memory* arrest_cj_chased_reply(GuestState& s, uint32_t, Memory* m) {
    uint32_t cop = ecx(s);
    uint32_t player = mem::read<uint32_t>(esi(s) + 0x10);
    int16_t result = say_original(s, cop, static_cast<uint16_t>(stack_arg(s, 0)));
    if (result >= 0 && player && !ped_vehicle(player)) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: CJ answers an arrest order with a CHASED line");
        ped_say(s, player, kContextChased, 3500, 0.6f, stack_arg(s, 3), stack_arg(s, 4), stack_arg(s, 5));
    }
    return_say(s, result);
    return m;
}

uint32_t g_shoot_target = 0;

uint32_t shoot_store_target(GuestState& s, uint32_t) {
    g_shoot_target = ebp(s);
    return 0x007427f6;
}

Memory* shoot_context_check_target(GuestState& s, uint32_t, Memory* m) {
    uint32_t target = std::exchange(g_shoot_target, 0u);
    if (!target) {
        target = mem::read<uint32_t>(ecx(s) + kMouseLockOnRecruitPed);
    }
    if (target && is_ped_entity(target)) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: CJ's SHOOT line plays for a ped target");
        eip(s) = redirect_original();
        return m;
    }
    return_say(s, -1);
    return m;
}

uint32_t cop_group_dummy() {
    static const uint32_t dummy = [] {
        uint32_t block = allocator().alloc(0x2d4);
        for (uint32_t i = 0; i < 0x2d4; i += 4) {
            mem::write<uint32_t>(block + i, 0);
        }
        return block;
    }();
    return dummy;
}

Memory* peds_group_for_cops(GuestState& s, uint32_t, Memory* m) {
    uint32_t ped = stack_arg(s, 0);
    if (ped && ped_type(ped) == 6) {
        uint32_t wanted = guest_call<Conv::cdecl_, uint32_t>(s, kFindPlayerWanted, 0xffffffffu);
        bool group = wanted && mem::read<uint8_t>(wanted + 0x18) > 1;
        if (group) {
            static std::atomic<bool> reached = false;
            log_once(reached, "speech: cops chasing CJ in a group can say SURROUNDED and COVER ME");
        }
        eax(s) = group ? cop_group_dummy() : 0;
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* dismiss_reply_where_you_from(GuestState& s, uint32_t, Memory* m) {
    uint32_t topic = mem::read<uint32_t>(kConversationTopic);
    if (topic == kConvGangbang) {
        return_say(s, -1);
        return m;
    }
    if (topic == kConvWhereYouFrom) {
        static std::atomic<bool> reached = false;
        log_once(reached, "speech: WHERE_YOU_FROM_POS gets its positive reply");
        mem::write<uint32_t>(float_arg(s, 0), kContextWhereYouFromPosReply);
    }
    eip(s) = redirect_original();
    return m;
}

Memory* drugged_ignore_on_homie(GuestState& s, uint32_t, Memory* m) {
    ecx(s) = edi(s);
    eip(s) = redirect_original();
    return m;
}

uint32_t g_chasing_cop = 0;

bool ped_fleeing_from_cops(GuestState& s, uint32_t ped) {
    if (mem::read<uint32_t>(ped + kPedFlags) & kBeingChasedByPolice) {
        g_chasing_cop = 0;
        return true;
    }
    uint32_t intelligence = mem::read<uint32_t>(ped + kPedIntelligence);
    uint32_t event = mem::read<uint32_t>(intelligence + 0x40);
    if (!event) {
        event = mem::read<uint32_t>(intelligence + 0x3c);
    }
    if (event) {
        uint32_t vtable = mem::read<uint32_t>(event);
        if (guest_call<Conv::thiscall, int32_t>(s, mem::read<uint32_t>(vtable + 4), event) == 26) {
            uint32_t cop = mem::read<uint32_t>(event + 0x0c);
            if (cop && ped_type(cop) == 6) {
                g_chasing_cop = cop;
                return true;
            }
        }
    }
    return false;
}

bool ped_not_dying_and_bonus_not_given(GuestState& s, uint32_t ped) {
    uint32_t flags = mem::read<uint32_t>(ped + kPedFlags);
    if (flags & kBeingChasedByPolice) {
        return false;
    }
    int32_t state = mem::read<int32_t>(ped + kPedState);
    if (state == 54 || state == 55) {
        return false;
    }
    mem::write<uint32_t>(ped + kPedFlags, flags | kBeingChasedByPolice);
    if (g_chasing_cop) {
        ped_say(s, g_chasing_cop, kContextGoodCitizen);
    }
    return true;
}

uint32_t good_citizen_check(GuestState& s, uint32_t) {
    uint32_t ped = edi(s);
    SavedScratch saved(s);
    if (!ped_fleeing_from_cops(s, ped)) {
        return 0x00532102;
    }
    if (!ped_not_dying_and_bonus_not_given(s, ped)) {
        return 0x00532226;
    }
    std::fprintf(stderr, "[SP] good citizen: bonus for stopping ped 0x%08x fleeing from cop 0x%08x\n", ped, g_chasing_cop);
    return 0x005320b6;
}

Memory* good_citizen_text(GuestState& s, uint32_t, Memory* m) {
    static const uint32_t fallback = guest_literal("$50 Good Citizen Bonus!");
    uint32_t text = guest_call<Conv::thiscall, uint32_t>(s, redirect_original(), ecx(s), stack_arg(s, 0));
    if (!text || mem::read<uint8_t>(text) == 0) {
        text = fallback;
    }
    eax(s) = text;
    return_to_caller(s, 4);
    return m;
}

uint32_t shadow_point_transform(GuestState& s, uint32_t pc) {
    uint32_t entity = edi(s);
    uint32_t matrix = mem::read<uint32_t>(entity + 0x14);
    if (!matrix) {
        return pc;
    }
    uint32_t vec = esi(s) - 4;
    float x = mem::read<float>(vec);
    float y = mem::read<float>(vec + 4);
    float z = mem::read<float>(vec + 8);
    for (uint32_t i = 0; i < 3; ++i) {
        float v = mem::read<float>(matrix + 4 * i) * x + mem::read<float>(matrix + 0x10 + 4 * i) * y +
                  mem::read<float>(matrix + 0x20 + 4 * i) * z + mem::read<float>(matrix + 0x30 + 4 * i);
        mem::write<float>(vec + 4 * i, v);
    }
    static std::atomic<bool> reached = false;
    log_once(reached, "shadows: CastShadowEntityXY transforms points by the full entity matrix");
    return 0x007098c3;
}

Memory* radio_name_centre_size(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    guest_call<Conv::cdecl_>(s, redirect_original(), stack_arg(s, 0));
    guest_call<Conv::cdecl_>(s, kSetCentreSize, screen_width());
    log_once(reached, "radio: the station name's centre size is reset to the screen width");
    return_to_caller(s);
    return m;
}

Memory* fort_carson_beagle(GuestState& s, uint32_t, Memory* m) {
    static constexpr uint8_t kBeagle[] = {0xde, 0xd3, 0x1a, 0xc2, 0x66, 0x66, 0x87, 0x44, 0x63, 0x7f,
                                          0x99, 0x41, 0xd5, 0xb8, 0xb2, 0x3d, 0xff, 0x01, 0x00, 0x00};
    uint32_t cargen = stack_arg(s, 0);
    bool match = true;
    for (uint32_t i = 0; i < sizeof(kBeagle) && match; ++i) {
        match = mem::read<uint8_t>(cargen + i) == kBeagle[i];
    }
    if (match) {
        mem::write<int32_t>(cargen + 0x10, 481);
        std::fprintf(stderr, "[SP] car generator: the Fort Carson Beagle is a BMX\n");
    }
    eip(s) = redirect_original();
    return m;
}

constexpr uint32_t kTimeStep = 0x00b7cb5c;
constexpr uint32_t kModelInfoPtrs = 0x00a9b0c8;
constexpr uint32_t kGetFrameHierarchyId = 0x00732a20;
constexpr uint32_t kRwFrameGetLTM = 0x007f0990;
constexpr uint32_t kRpClumpForAllAtomics = 0x00749b70;
constexpr uint32_t kSetEditableMaterialsCB = 0x004c83e0;
constexpr uint32_t kEntityRender = 0x00534310;
constexpr uint32_t kCarNodes = 0x648;
constexpr uint32_t kGunOrientation = 0x94c;
constexpr uint32_t kSpecialComponentAngle = 0x958;
constexpr uint32_t kHeightAboveRoad = 0x898;
constexpr uint32_t kEngineCompOperands[] = {0x006ac2c0, 0x006acb93};
constexpr float kPi = 3.14159265358979323846f;

enum class Svf { phoenix_flutter, sweeper_brushes, newsvan_dish, extra_ailerons1, extra_ailerons2, double_trailer, firela_ladder, no_rotor_fade };

bool svf_has(int32_t model, Svf feature) {
    static constexpr std::pair<int32_t, Svf> kStock[] = {
        {511, Svf::extra_ailerons1}, {513, Svf::extra_ailerons2}, {544, Svf::firela_ladder}, {574, Svf::sweeper_brushes},
        {582, Svf::newsvan_dish},    {591, Svf::double_trailer},  {603, Svf::phoenix_flutter},
        {487, Svf::no_rotor_fade},   {460, Svf::no_rotor_fade},
    };
    return std::ranges::any_of(kStock, [&](const auto& f) { return f.first == model && f.second == feature; });
}

int32_t entity_model(uint32_t entity) { return mem::read<int16_t>(entity + 0x22); }

uint32_t car_node(uint32_t vehicle, uint32_t index) { return mem::read<uint32_t>(vehicle + kCarNodes + 4 * index); }

bool engine_on(uint32_t vehicle) { return mem::read<uint8_t>(vehicle + 0x428) & 0x10; }

uint32_t entity_status(uint32_t entity) { return mem::read<uint8_t>(entity + 0x36) >> 3; }

bool driven_status(uint32_t vehicle) {
    uint32_t status = entity_status(vehicle);
    return status == 0 || status == 3 || status == 2;
}

struct Mat {
    std::array<float, 3> right, up, at, pos;
};

Mat read_frame_matrix(uint32_t frame) {
    uint32_t m = frame + 0x10;
    Mat r{};
    for (uint32_t i = 0; i < 3; ++i) {
        r.right[i] = mem::read<float>(m + 4 * i);
        r.up[i] = mem::read<float>(m + 0x10 + 4 * i);
        r.at[i] = mem::read<float>(m + 0x20 + 4 * i);
        r.pos[i] = mem::read<float>(m + 0x30 + 4 * i);
    }
    return r;
}

void write_frame_matrix(uint32_t frame, const Mat& r) {
    uint32_t m = frame + 0x10;
    for (uint32_t i = 0; i < 3; ++i) {
        mem::write<float>(m + 4 * i, r.right[i]);
        mem::write<float>(m + 0x10 + 4 * i, r.up[i]);
        mem::write<float>(m + 0x20 + 4 * i, r.at[i]);
        mem::write<float>(m + 0x30 + 4 * i, r.pos[i]);
    }
    mem::write<uint32_t>(m + 0x0c, mem::read<uint32_t>(m + 0x0c) & ~0x20003u);
}

Mat rotation(int axis, float angle) {
    float c = std::cos(angle);
    float s = std::sin(angle);
    Mat r{};
    if (axis == 0) {
        r.right = {1.0f, 0.0f, 0.0f};
        r.up = {0.0f, c, s};
        r.at = {0.0f, -s, c};
    } else {
        r.at = {0.0f, 0.0f, 1.0f};
        r.up = {-s, c, 0.0f};
        r.right = {c, s, 0.0f};
    }
    r.pos = {0.0f, 0.0f, 0.0f};
    return r;
}

std::array<float, 3> times(const std::array<float, 3>& v, const Mat& r) {
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = v[0] * r.right[i] + v[1] * r.up[i] + v[2] * r.at[i] + r.pos[i];
    }
    return out;
}

void set_component_rotation(uint32_t frame, int axis, float angle, bool absolute) {
    if (!frame) {
        return;
    }
    Mat m = read_frame_matrix(frame);
    Mat r = rotation(axis, angle);
    if (absolute) {
        m.right = r.right;
        m.up = r.up;
        m.at = r.at;
    } else {
        auto pos = m.pos;
        m.pos = {0.0f, 0.0f, 0.0f};
        Mat out{times(m.right, r), times(m.up, r), times(m.at, r), times(m.pos, r)};
        for (int i = 0; i < 3; ++i) {
            out.pos[i] += pos[i];
        }
        m = out;
    }
    write_frame_matrix(frame, m);
}

uint32_t frame_from_id(GuestState& s, uint32_t top, int32_t id) {
    for (uint32_t child = mem::read<uint32_t>(top + 0x98); child; child = mem::read<uint32_t>(child + 0x9c)) {
        if (guest_call<Conv::cdecl_, int32_t>(s, kGetFrameHierarchyId, child) == id) {
            return child;
        }
        if (uint32_t found = frame_from_id(s, child, id)) {
            return found;
        }
    }
    return 0;
}

uint32_t original_clump_frame(int32_t model) {
    uint32_t info = mem::read<uint32_t>(kModelInfoPtrs + 4 * static_cast<uint32_t>(model));
    uint32_t clump = info ? mem::read<uint32_t>(info + 0x1c) : 0;
    return clump ? mem::read<uint32_t>(clump + 4) : 0;
}

float g_engine_comp_speed = 0.0f;

float engine_comp_speed(float) { return g_engine_comp_speed; }

void process_phoenix_blower(GuestState& s, uint32_t car, int32_t model) {
    uint32_t node = car_node(car, 20);
    if (!node || !engine_on(car)) {
        return;
    }
    if (uint32_t top = original_clump_frame(model)) {
        if (uint32_t orig = frame_from_id(s, top, 20)) {
            for (uint32_t i = 0; i < 0x40; i += 4) {
                mem::write<uint32_t>(node + 0x10 + i, mem::read<uint32_t>(orig + 0x10 + i));
            }
        }
    }
    float step = mem::read<float>(kTimeStep);
    float special = mem::read<float>(car + kSpecialComponentAngle);
    float final_angle = 0.0f;
    if (std::abs(mem::read<float>(car + 0x49c)) > 0.0f) {
        if (special < 1.3f) {
            special = std::min(special + 0.1f * step, 1.3f);
            final_angle = special;
        } else {
            float t = static_cast<float>(mem::read<uint32_t>(0x00b7cb84) % 10000) / 70.0f;
            final_angle = special + std::sin(t) * 0.13f;
        }
    } else if (special > 0.0f) {
        special = std::max(special - 0.05f * step, 0.0f);
        final_angle = special;
    }
    mem::write<float>(car + kSpecialComponentAngle, special);
    set_component_rotation(node, 0, final_angle, false);
}

void after_automobile_pre_render(GuestState& s, uint32_t car) {
    static std::atomic<bool> phoenix = false;
    static std::atomic<bool> sweeper = false;
    static std::atomic<bool> newsvan = false;
    int32_t model = entity_model(car);
    float step = mem::read<float>(kTimeStep);
    if (svf_has(model, Svf::phoenix_flutter)) {
        process_phoenix_blower(s, car, model);
        log_once(phoenix, "vehicle animations: Phoenix hood scoops animated");
    }
    if (svf_has(model, Svf::sweeper_brushes) && engine_on(car) && driven_status(car)) {
        float angle = step * 0.3f;
        set_component_rotation(car_node(car, 20), 2, angle, false);
        set_component_rotation(car_node(car, 21), 2, -angle, false);
        log_once(sweeper, "vehicle animations: Sweeper brushes animated");
    }
    if (svf_has(model, Svf::newsvan_dish) && driven_status(car)) {
        float angle = mem::read<float>(car + kGunOrientation) + step * 0.05f;
        if (angle > 2.0f * kPi) {
            angle -= 2.0f * kPi;
        }
        mem::write<float>(car + kGunOrientation, angle);
        set_component_rotation(car_node(car, 20), 2, angle, true);
        log_once(newsvan, "vehicle animations: Newsvan antenna animated");
    }
}

Memory* automobile_pre_render(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t car = ecx(s);
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] vehicle animations: CAutomobile::PreRender wrapped (site 0x%08x, model %d)\n", redirect_site(),
                     entity_model(car));
    }
    g_engine_comp_speed = engine_on(car) ? mem::read<float>(kTimeStep) : 0.0f;
    for (uint32_t operand : kEngineCompOperands) {
        mem::write<float>(mem::read<uint32_t>(operand), g_engine_comp_speed);
    }
    guest_call<Conv::thiscall>(s, redirect_original(), car);
    after_automobile_pre_render(s, car);
    return_to_caller(s);
    return m;
}

void copy_frame_rotation(uint32_t plane, uint32_t src, uint32_t dst) {
    uint32_t from = car_node(plane, src);
    uint32_t to = car_node(plane, dst);
    if (from && to) {
        Mat a = read_frame_matrix(from);
        Mat b = read_frame_matrix(to);
        b.right = a.right;
        b.up = a.up;
        b.at = a.at;
        write_frame_matrix(to, b);
    }
}

Memory* plane_pre_render(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t plane = ecx(s);
    guest_call<Conv::thiscall>(s, redirect_original(), plane);
    int32_t model = entity_model(plane);
    if (svf_has(model, Svf::extra_ailerons1)) {
        copy_frame_rotation(plane, 18, 21);
        log_once(reached, "vehicle animations: extra plane flaps follow the ailerons");
    }
    if (svf_has(model, Svf::extra_ailerons2)) {
        copy_frame_rotation(plane, 19, 23);
        copy_frame_rotation(plane, 20, 24);
        log_once(reached, "vehicle animations: extra plane flaps follow the ailerons");
    }
    return_to_caller(s);
    return m;
}

Memory* trailer_tow_bar_pos(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t trailer = ecx(s);
    uint32_t out = stack_arg(s, 0);
    bool default_pos = (stack_arg(s, 1) & 0xff) != 0;
    int32_t model = entity_model(trailer);
    if (svf_has(model, Svf::double_trailer)) {
        if (uint32_t hook = car_node(trailer, 21)) {
            uint32_t ltm = guest_call<Conv::cdecl_, uint32_t>(s, kRwFrameGetLTM, hook);
            for (uint32_t i = 0; i < 3; ++i) {
                mem::write<float>(out + 4 * i, mem::read<float>(ltm + 0x30 + 4 * i));
            }
            log_once(reached, "trailers: artict3's tow bar is its hook dummy, so it can tow another trailer");
            eax(s) = 1;
            return_to_caller(s, 12);
            return m;
        }
        uint32_t matrix = mem::read<uint32_t>(trailer + 0x14);
        uint32_t info = mem::read<uint32_t>(kModelInfoPtrs + 4 * static_cast<uint32_t>(model));
        uint32_t col = info ? mem::read<uint32_t>(info + 0x14) : 0;
        if (default_pos && matrix && col) {
            std::array<float, 3> v = {0.0f, mem::read<float>(col + 4) - 0.05f, 0.5f - mem::read<float>(trailer + kHeightAboveRoad)};
            for (uint32_t i = 0; i < 3; ++i) {
                float r = mem::read<float>(matrix + 4 * i) * v[0] + mem::read<float>(matrix + 0x10 + 4 * i) * v[1] +
                          mem::read<float>(matrix + 0x20 + 4 * i) * v[2] + mem::read<float>(matrix + 0x30 + 4 * i);
                mem::write<float>(out + 4 * i, r);
            }
            eax(s) = 1;
            return_to_caller(s, 12);
            return m;
        }
    }
    eip(s) = redirect_original();
    return m;
}

uint32_t firela_hydraulics_test(GuestState& s, uint32_t pc) {
    if (svf_has(entity_model(esi(s)), Svf::firela_ladder)) {
        static std::atomic<bool> reached = false;
        log_once(reached, "firela: the ladder takes the hydraulics controls");
        return 0x006b200f;
    }
    return pc;
}

uint32_t firela_camera_control(GuestState& s, uint32_t pc) {
    return svf_has(entity_model(edi(s)), Svf::firela_ladder) ? 0x0052551e : pc;
}

uint32_t tug_tow_bar_frame(GuestState& s, uint32_t) {
    uint32_t car = ecx(s);
    uint32_t bar = car_node(car, 20);
    if (!bar) {
        bar = car_node(car, 21);
        if (bar) {
            static std::atomic<bool> reached = false;
            log_once(reached, "tow bar: misc_b is used when misc_a is absent (Tug)");
        }
    }
    eax(s) = bar;
    return 0x006af2d2;
}

int compare_ignore_case(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int x = std::tolower(static_cast<unsigned char>(a[i]));
        int y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) {
            return x - y;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

Memory* hierarchy_typos(GuestState& s, uint32_t, Memory* m) {
    static constexpr std::pair<std::string_view, std::string_view> kTypos[] = {
        {"boat_moving_hi", "boat_moving"}, {"elevator_r", "elevator"},          {"misc_a", "misca"},
        {"misc_b", "miscb"},               {"taillights", "tailights"},         {"taillights2", "tailights2"},
        {"transmission_f", "transmision_f"}, {"transmission_r", "transmision_r"}, {"wheel_lm_dummy", "wheel_lm"},
    };
    uint32_t data_name = stack_arg(s, 0);
    uint32_t node_name = stack_arg(s, 1);
    auto result = guest_call<Conv::cdecl_, int32_t>(s, redirect_original(), data_name, node_name);
    if (result != 0) {
        std::string data = mem::read_cstr(data_name, 64);
        std::string node = mem::read_cstr(node_name, 64);
        for (const auto& [typo, spelt] : kTypos) {
            int comp = compare_ignore_case(spelt, node);
            if (comp > 0) {
                break;
            }
            if (comp == 0 && compare_ignore_case(typo, data) == 0) {
                std::fprintf(stderr, "[SP] hierarchy: node %s matches %s\n", node.c_str(), data.c_str());
                result = 0;
                break;
            }
        }
    }
    eax(s) = static_cast<uint32_t>(result);
    return_to_caller(s);
    return m;
}

uint32_t detached_part_render(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    static const uint32_t buffer = allocator().alloc(256 * 8 + 4);
    static const uint32_t saved_cull = allocator().alloc(4);
    constexpr uint32_t kCullMode = 20;
    constexpr uint32_t kCullNone = 1;
    uint32_t object = esi(s);
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    uint32_t set = mem::read<uint32_t>(engine + 0x20);
    uint32_t get = mem::read<uint32_t>(engine + 0x24);
    uint32_t cursor = buffer + 256 * 8;
    mem::write<uint32_t>(cursor, buffer);
    SavedScratch saved(s);
    guest_call<Conv::cdecl_>(s, get, kCullMode, saved_cull);
    uint32_t rw = mem::read<uint32_t>(object + 0x18);
    bool part = rw && mem::read<int16_t>(object + 0x14a) != -1 && mem::read<uint8_t>(object + 0x13c) == 3 &&
                (mem::read<uint8_t>(object + 0x140) & 0x80);
    if (part) {
        if (mem::read<uint8_t>(rw) == 1) {
            guest_call<Conv::cdecl_>(s, kSetEditableMaterialsCB, rw, cursor);
        } else {
            guest_call<Conv::cdecl_>(s, kRpClumpForAllAtomics, rw, kSetEditableMaterialsCB, cursor);
        }
        guest_call<Conv::cdecl_>(s, set, kCullMode, kCullNone);
        log_once(reached, "detached parts: drawn with the vehicle's colours and without backface culling");
    }
    guest_call<Conv::thiscall>(s, kEntityRender, object);
    for (uint32_t pair = buffer; pair < mem::read<uint32_t>(cursor); pair += 8) {
        mem::write<uint32_t>(mem::read<uint32_t>(pair), mem::read<uint32_t>(pair + 4));
    }
    guest_call<Conv::cdecl_>(s, set, kCullMode, mem::read<uint32_t>(saved_cull));
    return 0x0059f1f2;
}

Memory* lit_flying_component(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t entity = stack_arg(s, 0);
    mem::write<uint32_t>(entity + 0x1c, mem::read<uint32_t>(entity + 0x1c) | 0x10000000);
    log_once(reached, "detached parts: flying components get directional lights");
    eip(s) = redirect_original();
    return m;
}

constexpr uint32_t kDirtTextures = 0x00c02bd0;
constexpr uint32_t kRpMaterialSetTexture = 0x0074dbc0;
constexpr uint32_t kGeneratePlateText = 0x006fd5b0;
constexpr uint32_t kCreatePlateTexture = 0x006fdea0;
constexpr uint32_t kCurrentMapRegion = 0x00c81314;
constexpr uint32_t kSetupMaterialPlatebackTexture = 0x006fde50;
constexpr uint32_t kIsLawEnforcementVehicle = 0x006d2370;
constexpr uint32_t kDirectionalLight = 0x00c886ec;
constexpr uint32_t kStaticDirtMaterials = 0x230;
constexpr uint32_t kDirtMaterialsPtr = 0x2a8;
constexpr uint32_t kNumDirtMaterials = 0x2ac;
constexpr uint32_t kInPlaceDirt = 30;
constexpr uint32_t kPlateText = 0x28;
constexpr uint32_t kPlateType = 0x31;
constexpr uint32_t kPlateDesign = 0x587;
constexpr uint32_t kPlateTexture = 0x588;

template <typename Fn>
void for_each_clump_atomic(uint32_t clump, Fn&& fn) {
    if (!clump) {
        return;
    }
    uint32_t head = clump + 0x08;
    for (uint32_t link = mem::read<uint32_t>(head); link != head;) {
        uint32_t next = mem::read<uint32_t>(link);
        fn(link - 0x40);
        link = next;
    }
}

template <typename Fn>
void for_each_atomic_material(uint32_t atomic, Fn&& fn) {
    uint32_t geometry = atomic ? mem::read<uint32_t>(atomic + 0x18) : 0;
    if (!geometry) {
        return;
    }
    uint32_t materials = mem::read<uint32_t>(geometry + 0x20);
    int32_t count = mem::read<int32_t>(geometry + 0x24);
    for (int32_t i = 0; i < count; ++i) {
        fn(mem::read<uint32_t>(materials + 4 * static_cast<uint32_t>(i)));
    }
}

std::string material_texture_name(uint32_t material) {
    uint32_t texture = material ? mem::read<uint32_t>(material) : 0;
    return texture ? mem::read_cstr(texture + 0x10, 32) : std::string();
}

std::map<uint32_t, std::vector<uint32_t>>& extra_dirt_materials() {
    static std::map<uint32_t, std::vector<uint32_t>> extra;
    return extra;
}

uint32_t find_editable_material_list(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    uint32_t info = esi(s);
    std::vector<uint32_t> found;
    auto collect = [&](uint32_t atomic) {
        for_each_atomic_material(atomic, [&](uint32_t material) {
            if (material_texture_name(material) == "vehiclegrunge256") {
                found.push_back(material);
            }
        });
    };
    for_each_clump_atomic(mem::read<uint32_t>(info + 0x1c), collect);
    if (uint32_t structure = mem::read<uint32_t>(info + 0x5c)) {
        for (uint32_t i = 0; i < mem::read<uint8_t>(structure + 0x30c); ++i) {
            collect(mem::read<uint32_t>(structure + 0x2f4 + 4 * i));
        }
    }
    uint32_t in_place = std::min<uint32_t>(static_cast<uint32_t>(found.size()), kInPlaceDirt);
    for (uint32_t i = 0; i < in_place; ++i) {
        mem::write<uint32_t>(info + kStaticDirtMaterials + 4 * i, found[i]);
    }
    mem::write<uint32_t>(info + kDirtMaterialsPtr, 0);
    mem::write<uint32_t>(info + kNumDirtMaterials, static_cast<uint32_t>(found.size()));
    if (found.size() > in_place) {
        extra_dirt_materials()[info].assign(found.begin() + in_place, found.end());
    } else {
        extra_dirt_materials().erase(info);
    }
    for (uint32_t c = 0x2d2; c <= 0x2d5; ++c) {
        mem::write<int8_t>(info + c, -1);
    }
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] dirt: model info 0x%08x has %zu dirt materials\n", info, found.size());
    }
    ecx(s) = info;
    return 0x004c9660;
}

Memory* remap_dirt(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t info = stack_arg(s, 0);
    uint32_t level = stack_arg(s, 1);
    uint32_t texture = mem::read<uint32_t>(kDirtTextures + 4 * level);
    uint32_t count = mem::read<uint32_t>(info + kNumDirtMaterials);
    for (uint32_t i = 0; i < std::min(count, kInPlaceDirt); ++i) {
        guest_call<Conv::cdecl_>(s, kRpMaterialSetTexture, mem::read<uint32_t>(info + kStaticDirtMaterials + 4 * i), texture);
    }
    auto it = extra_dirt_materials().find(info);
    if (it != extra_dirt_materials().end()) {
        for (uint32_t material : it->second) {
            guest_call<Conv::cdecl_>(s, kRpMaterialSetTexture, material, texture);
        }
    }
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] dirt: %u dirt materials remapped to level %u\n", count, level);
    }
    return_to_caller(s);
    return m;
}

Memory* vehicle_model_shutdown(GuestState& s, uint32_t, Memory* m) {
    extra_dirt_materials().erase(ecx(s));
    mem::write<uint32_t>(ecx(s) + kDirtMaterialsPtr, 0);
    eip(s) = redirect_original();
    return m;
}

Memory* set_car_custom_plate(GuestState& s, uint32_t, Memory* m) {
    uint32_t info = ecx(s);
    mem::write<uint8_t>(info + kPlateText, 0);
    mem::write<int8_t>(info + kPlateType, -1);
    return_to_caller(s);
    return m;
}

int8_t map_region_plate_design() {
    int32_t region = mem::read<int16_t>(kCurrentMapRegion);
    if (region == 1) {
        return 2;
    }
    return region >= 3 && region <= 4 ? 1 : 0;
}

int32_t ps2_random(int32_t from, int32_t to) {
    static uint64_t seed = static_cast<uint64_t>(std::time(nullptr));
    seed = 0x5851F42D4C957F2Dull * seed + 1;
    return from + static_cast<int32_t>((seed >> 32) & 0x7fffffff) % (to - from);
}

Memory* plate_texture_create(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    static const uint32_t buffer = allocator().alloc(16);
    uint32_t vehicle = ecx(s);
    uint32_t info = stack_arg(s, 0);
    if (mem::read<uint8_t>(info + kPlateText)) {
        for (uint32_t i = 0; i < 8; ++i) {
            mem::write<uint8_t>(buffer + i, mem::read<uint8_t>(info + kPlateText + i));
        }
    } else {
        guest_call<Conv::cdecl_>(s, kGeneratePlateText, buffer, 8u);
    }
    mem::write<uint8_t>(buffer + 8, 0);
    auto type = mem::read<int8_t>(info + kPlateType);
    uint32_t texture = guest_call<Conv::cdecl_, uint32_t>(s, kCreatePlateTexture, buffer, static_cast<int32_t>(type));
    mem::write<uint32_t>(vehicle + kPlateTexture, texture);
    int8_t design = 0;
    if (type != -1) {
        design = type;
    } else if (guest_call<Conv::thiscall, uint8_t>(s, kIsLawEnforcementVehicle, vehicle)) {
        design = map_region_plate_design();
    } else {
        design = ps2_random(0, 20) == 0 ? static_cast<int8_t>(ps2_random(0, 3)) : map_region_plate_design();
    }
    mem::write<int8_t>(vehicle + kPlateDesign, design);
    mem::write<uint8_t>(info + kPlateText, 0);
    mem::write<int8_t>(info + kPlateType, -1);
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] number plates: vehicle 0x%08x gets its own plate \"%s\" (design %d)\n", vehicle,
                     mem::read_cstr(buffer, 9).c_str(), design);
    }
    eax(s) = 1;
    return_to_caller(s, 4);
    return m;
}

std::vector<std::pair<uint32_t, uint32_t>>& original_plate_materials() {
    static std::vector<std::pair<uint32_t, uint32_t>> saved;
    return saved;
}

Memory* plate_before_render(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t vehicle = ecx(s);
    uint32_t texture = mem::read<uint32_t>(vehicle + kPlateTexture);
    auto design = mem::read<int8_t>(vehicle + kPlateDesign);
    std::vector<std::pair<uint32_t, int>> changes;
    for_each_clump_atomic(mem::read<uint32_t>(vehicle + 0x18), [&](uint32_t atomic) {
        for_each_atomic_material(atomic, [&](uint32_t material) {
            std::string name = material_texture_name(material);
            if (name == "carplate") {
                changes.emplace_back(material, 0);
            } else if (name == "carpback") {
                changes.emplace_back(material, 1);
            }
        });
    });
    for (auto [material, kind] : changes) {
        original_plate_materials().emplace_back(material, mem::read<uint32_t>(material));
        if (kind == 0) {
            guest_call<Conv::cdecl_>(s, kRpMaterialSetTexture, material, texture);
        } else {
            guest_call<Conv::cdecl_>(s, kSetupMaterialPlatebackTexture, material, static_cast<int32_t>(design));
        }
    }
    if (!changes.empty() && mem::read<uint32_t>(vehicle + 0x590) == 9) {
        log_once(reached, "number plates: a bike shows its custom plate");
    }
    return_to_caller(s, 4);
    return m;
}

Memory* plate_after_render(GuestState& s, uint32_t, Memory* m) {
    for (auto [material, texture] : original_plate_materials()) {
        guest_call<Conv::cdecl_>(s, kRpMaterialSetTexture, material, texture);
    }
    original_plate_materials().clear();
    return_to_caller(s, 4);
    return m;
}

Memory* plate_setup_after_upgrade(GuestState& s, uint32_t, Memory* m) {
    eax(s) = 0;
    return_to_caller(s);
    return m;
}

uint32_t road_reflection_depth(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    log_once(reached, "road reflections: the reflection quad uses depth 1.0");
    x87_push(s, 1.0);
    return 0x006fb9a1;
}

uint32_t light_materials_save(GuestState& s, uint32_t) {
    uint32_t cursor = ecx(s);
    uint32_t surface = edx(s);
    mem::write<uint32_t>(esi(s), edi(s));
    uint32_t out = mem::read<uint32_t>(cursor);
    mem::write<uint32_t>(out + 4, surface + 4);
    mem::write<uint32_t>(out + 8, mem::read<uint32_t>(surface + 4));
    mem::write<uint32_t>(out + 12, surface + 8);
    mem::write<uint32_t>(out + 16, mem::read<uint32_t>(surface + 8));
    mem::write<uint32_t>(cursor, out + 20);
    ebx(s) = out + 20;
    esi(s) = surface + 8;
    edi(s) = mem::read<uint32_t>(surface + 8);
    static std::atomic<bool> reached = false;
    log_once(reached, "lights: the material's ambient and diffuse values are saved for restore too");
    return 0x004c8311;
}

bool g_dark_vehicle = false;

Memory* dark_vehicle_check(GuestState& s, uint32_t, Memory* m) {
    uint32_t light = mem::read<uint32_t>(kDirectionalLight);
    g_dark_vehicle = ((eax(s) >> 14) & 1) && light && !(mem::read<uint8_t>(light + 2) & 1);
    if (g_dark_vehicle) {
        static std::atomic<bool> reached = false;
        log_once(reached, "blown-up cars: a wrecked car is drawn dark without the directional light's specular");
    }
    eip(s) = redirect_original();
    return m;
}

uint32_t dark_vehicle_branch_a(GuestState& s, uint32_t) {
    return (eax(s) == 0 || g_dark_vehicle) ? 0x005d9b09 : 0x005d9a7a;
}

uint32_t dark_vehicle_branch_b(GuestState& s, uint32_t) {
    return ((eax(s) & 0xff) == 0 || g_dark_vehicle) ? 0x005d9cac : 0x005d9b4a;
}

uint32_t dark_vehicle_branch_c(GuestState& s, uint32_t) {
    return ((eax(s) & 0xff) == 0 || g_dark_vehicle) ? 0x005d9e0d : 0x005d9cb8;
}

std::map<uint32_t, uint32_t>& lightbeam_saved_states() {
    static std::map<uint32_t, uint32_t> saved;
    return saved;
}

void render_state_set(GuestState& s, uint32_t state, uint32_t value) {
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    guest_call<Conv::cdecl_>(s, mem::read<uint32_t>(engine + 0x20), state, value);
}

uint32_t render_state_get(GuestState& s, uint32_t state) {
    static const uint32_t cell = allocator().alloc(4);
    uint32_t engine = mem::read<uint32_t>(kRwEngineInstance);
    guest_call<Conv::cdecl_>(s, mem::read<uint32_t>(engine + 0x24), state, cell);
    return mem::read<uint32_t>(cell);
}

uint32_t lightbeam_push_state(GuestState& s, uint32_t pc) {
    uint32_t state = mem::read<uint32_t>(esp(s));
    uint32_t value = mem::read<uint32_t>(esp(s) + 4);
    lightbeam_saved_states()[state] = render_state_get(s, state);
    render_state_set(s, state, value);
    eax(s) = 1;
    return pc + 3;
}

void lightbeam_pop(GuestState& s, uint32_t state) {
    auto& saved = lightbeam_saved_states();
    auto it = saved.find(state);
    if (it != saved.end()) {
        render_state_set(s, state, it->second);
    }
}

uint32_t lightbeam_pop_state(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    constexpr uint32_t kShadeMode = 7;
    constexpr uint32_t kDestBlend = 11;
    constexpr uint32_t kCullMode = 20;
    constexpr uint32_t kAlphaTestFunction = 29;
    constexpr uint32_t kAlphaTestFunctionRef = 30;
    uint32_t state = mem::read<uint32_t>(esp(s));
    lightbeam_pop(s, state);
    if (state == kDestBlend) {
        lightbeam_pop(s, kShadeMode);
    }
    if (state == kCullMode) {
        lightbeam_pop(s, kAlphaTestFunction);
        lightbeam_pop(s, kAlphaTestFunctionRef);
        log_once(reached, "light beam: render states restored after the head light beam, culling included");
    }
    eax(s) = 1;
    return pc + 3;
}

constexpr uint32_t kBouncingPanels = 0x6ac;
constexpr uint32_t kDoors = 0x5b8;

uint32_t first_frame_object(uint32_t frame) {
    uint32_t head = frame + 0x90;
    uint32_t link = mem::read<uint32_t>(head);
    return link != head ? link - 8 : 0;
}

void reset_bouncing_panels(uint32_t vehicle, uint32_t first) {
    for (uint32_t i = first; i < 3; ++i) {
        mem::write<int16_t>(vehicle + kBouncingPanels + 0x20 * i, -1);
    }
}

void reset_car_frames(GuestState& s, uint32_t car) {
    uint32_t top = original_clump_frame(entity_model(car));
    if (!top) {
        return;
    }
    for (int32_t i = 8; i < 25; ++i) {
        uint32_t node = car_node(car, static_cast<uint32_t>(i));
        if (!node) {
            continue;
        }
        if (uint32_t orig = frame_from_id(s, top, i)) {
            for (uint32_t b = 0; b < 0x40; b += 4) {
                mem::write<uint32_t>(node + 0x10 + b, mem::read<uint32_t>(orig + 0x10 + b));
            }
            mem::write<uint32_t>(node + 0x1c, mem::read<uint32_t>(node + 0x1c) & ~0x20003u);
        } else {
            Mat m = read_frame_matrix(node);
            m.right = {1.0f, 0.0f, 0.0f};
            m.up = {0.0f, 1.0f, 0.0f};
            m.at = {0.0f, 0.0f, 1.0f};
            write_frame_matrix(node, m);
        }
    }
}

uint32_t automobile_fix(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    uint32_t car = edi(s);
    int32_t model = entity_model(car);
    reset_car_frames(s, car);
    bool tow = (car_node(car, 21) && model == 525) || (car_node(car, 17) && model == 531);
    reset_bouncing_panels(car, tow ? 1 : 0);
    if (model == 432) {
        for (uint32_t door : {4u, 5u}) {
            uint32_t d = car + kDoors + 0x18 * door;
            mem::write<float>(d + 0x00, 1.0f);
            mem::write<float>(d + 0x04, 1.0f);
            mem::write<float>(d + 0x0c, 1.0f);
            mem::write<float>(d + 0x10, 1.0f);
        }
        for (uint32_t index : {3u, 6u}) {
            if (uint32_t node = car_node(car, index)) {
                if (uint32_t object = first_frame_object(node)) {
                    mem::write<uint8_t>(object + 2, 0);
                }
            }
        }
    }
    if (!reached.exchange(true)) {
        std::fprintf(stderr, "[SP] vehicle fix: CAutomobile::Fix resets frames from the model (model %d)\n", model);
    }
    uint32_t sp = esp(s);
    edi(s) = mem::read<uint32_t>(sp);
    esi(s) = mem::read<uint32_t>(sp + 4);
    ebp(s) = mem::read<uint32_t>(sp + 8);
    ebx(s) = mem::read<uint32_t>(sp + 12);
    esp(s) = sp + 16;
    return 0x006a356f;
}

uint32_t plane_fix(GuestState& s, uint32_t pc) {
    static std::atomic<bool> reached = false;
    uint32_t plane = edi(s);
    reset_bouncing_panels(plane, entity_model(plane) == 539 ? 1 : 0);
    log_once(reached, "vehicle fix: CPlane::Fix resets the bouncing panels");
    return pc;
}

constexpr uint32_t kWeaponPedsFirst = 0x00c88234;
constexpr uint32_t kWeaponPedsEnd = 0x00c88224;
constexpr uint32_t kGetAnimHierarchyFromSkinClump = 0x00734a40;
constexpr uint32_t kHAnimIDGetIndex = 0x007c51a0;
constexpr uint32_t kHAnimGetMatrixArray = 0x007c5120;
constexpr uint32_t kRwFrameUpdateObjects = 0x007f0910;
constexpr uint32_t kRpClumpRender = 0x00749b20;
constexpr uint32_t kRwMatrixTranslate = 0x007f2450;
constexpr uint32_t kRwMatrixRotate = 0x007f1fd0;
constexpr uint32_t kSetGunFlashAlpha = 0x005df400;
constexpr uint32_t kResetGunFlashAlpha = 0x005df4e0;
constexpr uint32_t kGetFirstObject = 0x007348c0;
constexpr uint32_t kGetWeaponInfo = 0x00743c60;
constexpr uint32_t kGetWeaponSkill = 0x005e6580;
constexpr uint32_t kAtomicDefaultRender = 0x007491c0;
constexpr uint32_t kParachuteOffset = 0x008d60b0;
constexpr uint32_t kParachuteAxis = 0x008d2338;
constexpr uint32_t kDualWeaponAxis = 0x008d60a4;
constexpr uint32_t kDualWeaponOffset = 0x008d6098;
constexpr uint32_t kRwCameraClear = 0x007ee340;
constexpr uint32_t kRwCameraBeginUpdate = 0x007ee190;
constexpr uint32_t kRwCameraEndUpdate = 0x007ee180;
constexpr uint32_t kShadowInvertRaster = 0x00705660;
constexpr uint32_t kGetTaskJetPack = 0x00601110;
constexpr uint32_t kRenderJetPack = 0x0067f6a0;
constexpr uint32_t kSetComponentAtomicAlpha = 0x006d2960;
constexpr uint32_t kFindPlayerVehicle = 0x0056e0d0;
constexpr uint32_t kCVehicleRender = 0x006d0e60;
constexpr uint32_t kBigVehicleAlphaCBOperand = 0x004c797b;
constexpr uint32_t kHeliRotorAlphaCBOperand = 0x004c7899;
constexpr uint32_t kHeliTailRotorAlphaCBOperand = 0x004c78b2;
constexpr uint32_t kCurrDisplayedSplash = 0x008d093c;
constexpr uint32_t kSplashesBegin = 0x00bab35c;
constexpr uint32_t kSplashesEnd = 0x00bab378;
constexpr uint32_t kLoadPickup = 0x005b47b0;
constexpr uint32_t kLoadCarGenerator = 0x005b4740;
constexpr uint32_t kLoadStuntJump = 0x005b45d0;
constexpr uint32_t kMadDriverTimerOperand = 0x0042136b;
constexpr uint32_t kPedWeaponObject = 0x4f4;
constexpr uint32_t kPedMuzzleFlashFrame = 0x4f8;
constexpr uint32_t kPedWeaponSlots = 0x5a0;
constexpr uint32_t kPedActiveWeapon = 0x718;
constexpr uint32_t kHeliRotorSpeed = 0x84c;
constexpr uint32_t kPlanePropellerSpeed = 0x9c4;
constexpr uint32_t kVehiclePassengers = 0x464;
constexpr uint32_t kVehicleRemapTxd = 0x598;
constexpr uint32_t kVehicleRemapTxdToLoad = 0x59a;
constexpr uint32_t kModelRemapTxds = 0x2fa;
constexpr uint32_t kModelVehicleType = 0x3c;
constexpr uint32_t kVehicleTypeTrailer = 11;
constexpr uint32_t kWeaponParachute = 46;
constexpr uint32_t kWeaponTwinPistols = 0x800;
constexpr uint32_t kRsZWrite = 8;
constexpr uint32_t kRsVertexAlpha = 12;
constexpr uint32_t kRsFog = 14;
constexpr uint32_t kRsAlphaRef = 30;
constexpr uint32_t kAtomicRender = 4;
constexpr uint32_t kShadowStrippedGeometryFlags = 0xfc;
constexpr uint16_t kContextCarHitPed = 36;
constexpr uint16_t kContextTaxiHitPed = 223;
constexpr uint32_t kBoolResetOperands[] = {0x0063e8d9, 0x0044ac98, 0x0044b49e};
constexpr uint32_t kIntResetOperands[] = {0x00421321, 0x0042131b, 0x0055c844, 0x0055ca3a, 0x0055cf40,
                                          0x0055cf2c, 0x0055cfc2, 0x0055c5e6, 0x0055d044};
constexpr uint32_t kTrueResetOperands[] = {0x004758a6, 0x004758c5};

void render_atomic(GuestState& s, uint32_t atomic) {
    guest_call<Conv::cdecl_, uint32_t>(s, mem::read<uint32_t>(atomic + 0x48), atomic);
}

void shadow_render_atomic(GuestState& s, uint32_t atomic) {
    if (!(mem::read<uint8_t>(atomic + 2) & kAtomicRender)) {
        return;
    }
    uint32_t geometry = mem::read<uint32_t>(atomic + 0x18);
    if (geometry && mem::read<uint32_t>(geometry + 0x58)) {
        uint32_t flags = mem::read<uint32_t>(geometry + 0x08);
        mem::write<uint32_t>(geometry + 0x08, flags & ~kShadowStrippedGeometryFlags);
        guest_call<Conv::cdecl_, uint32_t>(s, kAtomicDefaultRender, atomic);
        mem::write<uint32_t>(geometry + 0x08, flags);
    } else {
        guest_call<Conv::cdecl_, uint32_t>(s, kAtomicDefaultRender, atomic);
    }
}

void shadow_render_object(GuestState& s, uint32_t object) {
    if (!object) {
        return;
    }
    if (mem::read<uint8_t>(object) == 1) {
        shadow_render_atomic(s, object);
        return;
    }
    for_each_clump_atomic(object, [&](uint32_t atomic) { shadow_render_atomic(s, atomic); });
}

void copy_bone_matrix(GuestState& s, uint32_t hierarchy, uint32_t bone, uint32_t matrix) {
    auto index = guest_call<Conv::cdecl_, uint32_t>(s, kHAnimIDGetIndex, hierarchy, bone);
    uint32_t src = guest_call<Conv::cdecl_, uint32_t>(s, kHAnimGetMatrixArray, hierarchy) + 0x40 * index;
    for (uint32_t b = 0; b < 0x40; b += 4) {
        mem::write<uint32_t>(matrix + b, mem::read<uint32_t>(src + b));
    }
}

uint32_t active_weapon_type(uint32_t ped) {
    int32_t slot = mem::read<int8_t>(ped + kPedActiveWeapon);
    return mem::read<uint32_t>(ped + kPedWeaponSlots + static_cast<uint32_t>(slot * 0x1c));
}

void render_ped_weapon(GuestState& s, uint32_t ped, bool weapon, bool flash, bool shadow) {
    uint32_t object = mem::read<uint32_t>(ped + kPedWeaponObject);
    if (!object) {
        return;
    }
    uint32_t hierarchy = guest_call<Conv::cdecl_, uint32_t>(s, kGetAnimHierarchyFromSkinClump, mem::read<uint32_t>(ped + 0x18));
    if (!hierarchy) {
        return;
    }
    uint32_t type = active_weapon_type(ped);
    bool parachute = type == kWeaponParachute;
    uint32_t frame = mem::read<uint32_t>(object + 4);
    uint32_t matrix = frame + 0x10;
    copy_bone_matrix(s, hierarchy, parachute ? 3 : 24, matrix);
    auto render_one = [&](bool right) {
        guest_call<Conv::cdecl_, uint32_t>(s, kRwFrameUpdateObjects, frame);
        if (shadow) {
            shadow_render_object(s, object);
            return;
        }
        if (weapon) {
            guest_call<Conv::cdecl_, uint32_t>(s, kRpClumpRender, object);
        }
        uint32_t flash_frame = mem::read<uint32_t>(ped + kPedMuzzleFlashFrame);
        if (flash && flash_frame) {
            uint32_t zwrite = render_state_get(s, kRsZWrite);
            render_state_set(s, kRsZWrite, 0);
            guest_call<Conv::thiscall>(s, kSetGunFlashAlpha, ped, right ? 1u : 0u);
            if (uint32_t atomic = guest_call<Conv::cdecl_, uint32_t>(s, kGetFirstObject, flash_frame)) {
                render_atomic(s, atomic);
            }
            render_state_set(s, kRsZWrite, zwrite);
        }
    };
    if (parachute) {
        guest_call<Conv::cdecl_, uint32_t>(s, kRwMatrixTranslate, matrix, kParachuteOffset, 1u);
        guest_call<Conv::cdecl_, uint32_t>(s, kRwMatrixRotate, matrix, kParachuteAxis, 90.0f, 1u);
    }
    render_one(false);
    auto skill = guest_call<Conv::thiscall, uint8_t>(s, kGetWeaponSkill, ped);
    uint32_t info = guest_call<Conv::cdecl_, uint32_t>(s, kGetWeaponInfo, type, static_cast<uint32_t>(skill));
    if (info && (mem::read<uint32_t>(info + 0x18) & kWeaponTwinPistols)) {
        copy_bone_matrix(s, hierarchy, 34, matrix);
        guest_call<Conv::cdecl_, uint32_t>(s, kRwMatrixRotate, matrix, kDualWeaponAxis, 180.0f, 1u);
        guest_call<Conv::cdecl_, uint32_t>(s, kRwMatrixTranslate, matrix, kDualWeaponOffset, 1u);
        render_one(true);
    }
    if (flash) {
        guest_call<Conv::thiscall>(s, kResetGunFlashAlpha, ped);
    }
}

Memory* render_weapon_with_ped(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t ped = stack_arg(s, 0);
    if (mem::read<uint32_t>(ped + kPedWeaponObject)) {
        log_once(reached, "weapons: rendered with the ped, muzzle flash deferred");
    }
    render_ped_weapon(s, ped, true, false, false);
    guest_call<Conv::cdecl_>(s, original, ped);
    return_to_caller(s);
    return m;
}

Memory* render_weapon_peds_for_pc(GuestState& s, uint32_t, Memory* m) {
    constexpr uint32_t kStates[] = {kRsVertexAlpha, kRsZWrite, kRsFog};
    std::array<uint32_t, 3> saved{};
    for (size_t i = 0; i < saved.size(); ++i) {
        saved[i] = render_state_get(s, kStates[i]);
    }
    for (uint32_t state : kStates) {
        render_state_set(s, state, 1);
    }
    for (uint32_t node = mem::read<uint32_t>(kWeaponPedsFirst); node != kWeaponPedsEnd; node = mem::read<uint32_t>(node + 4)) {
        uint32_t ped = mem::read<uint32_t>(node);
        if (!ped) {
            continue;
        }
        uint32_t vtable = mem::read<uint32_t>(ped);
        auto lit = guest_call<Conv::thiscall, uint8_t>(s, mem::read<uint32_t>(vtable + 0x4c), ped);
        render_ped_weapon(s, ped, false, true, false);
        guest_call<Conv::thiscall>(s, mem::read<uint32_t>(vtable + 0x50), ped, static_cast<uint32_t>(lit));
    }
    for (size_t i = 0; i < saved.size(); ++i) {
        render_state_set(s, kStates[i], saved[i]);
    }
    return_to_caller(s);
    return m;
}

Memory* shadow_camera_update(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    static std::atomic<bool> reached_ped = false;
    static const uint32_t colour = [] {
        uint32_t c = allocator().alloc(4);
        mem::write<uint32_t>(c, 0x00ffffffu);
        return c;
    }();
    uint32_t shadow_camera = ecx(s);
    uint32_t entity = stack_arg(s, 0);
    uint32_t camera = mem::read<uint32_t>(shadow_camera);
    guest_call<Conv::cdecl_, uint32_t>(s, kRwCameraClear, camera, colour, 3u);
    if (guest_call<Conv::cdecl_, uint32_t>(s, kRwCameraBeginUpdate, camera)) {
        if (entity) {
            if (is_ped_entity(entity)) {
                shadow_render_object(s, mem::read<uint32_t>(entity + 0x18));
                if (mem::read<uint32_t>(entity + kPedWeaponObject)) {
                    log_once(reached_ped, "shadows: a ped's weapon is drawn into its shadow");
                }
                render_ped_weapon(s, entity, true, false, true);
                if (uint32_t intelligence = mem::read<uint32_t>(entity + kPedIntelligence)) {
                    if (uint32_t task = guest_call<Conv::thiscall, uint32_t>(s, kGetTaskJetPack, intelligence)) {
                        guest_call<Conv::thiscall>(s, kRenderJetPack, task, entity);
                    }
                }
            } else {
                shadow_render_object(s, mem::read<uint32_t>(entity + 0x18));
            }
        }
        guest_call<Conv::thiscall>(s, kShadowInvertRaster, shadow_camera);
        guest_call<Conv::cdecl_, uint32_t>(s, kRwCameraEndUpdate, camera);
    }
    log_once(reached, "shadows: CShadowCamera::Update takes the entity");
    eax(s) = camera;
    return_to_caller(s, 4);
    return m;
}

bool rotor_fix_enabled() {
    static const bool on = mod_settings().flag("silentpatch.rotor_fix");
    return on;
}

uint32_t current_atomic_object(uint32_t frame) {
    uint32_t head = frame + 0x90;
    for (uint32_t link = mem::read<uint32_t>(head); link != head; link = mem::read<uint32_t>(link)) {
        uint32_t object = link - 8;
        if (mem::read<uint8_t>(object + 2) & kAtomicRender) {
            return object;
        }
    }
    return 0;
}

void set_rotor_alphas(GuestState& s, uint32_t vehicle, float speed, double full_speed) {
    bool display = rotor_fix_enabled() && !svf_has(entity_model(vehicle), Svf::no_rotor_fade);
    bool moving1 = car_node(vehicle, 13) && display;
    bool moving2 = car_node(vehicle, 15) && display;
    double rotors = speed > 0.0f ? std::min(1.7 * (1.0 / full_speed) * speed, 1.5) : 0.0;
    double moving = std::max(rotors - 0.4, 0.0);
    auto static_alpha = static_cast<int32_t>(std::min((1.5 - rotors) * 255.0, 255.0));
    auto moving_alpha = static_cast<int32_t>(std::min(moving * 175.0, 175.0));
    auto apply = [&](uint32_t node, int32_t alpha) {
        if (uint32_t frame = car_node(vehicle, node)) {
            if (uint32_t atomic = current_atomic_object(frame)) {
                guest_call<Conv::cdecl_>(s, kSetComponentAtomicAlpha, atomic, static_cast<uint32_t>(alpha));
            }
        }
    };
    apply(12, moving1 ? static_alpha : 255);
    apply(14, moving2 ? static_alpha : 255);
    apply(13, moving1 ? moving_alpha : 0);
    apply(15, moving2 ? moving_alpha : 0);
}

Memory* heli_render_rotors(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t heli = ecx(s);
    set_rotor_alphas(s, heli, mem::read<float>(heli + kHeliRotorSpeed), 0.22);
    log_once(reached, "rotors: helicopter rotor alphas set before CEntity::Render");
    guest_call<Conv::thiscall>(s, original, heli);
    return_to_caller(s);
    return m;
}

uint32_t plane_render_rotors(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    uint32_t plane = ecx(s);
    set_rotor_alphas(s, plane, mem::read<float>(plane + kPlanePropellerSpeed), 0.31);
    log_once(reached, "rotors: plane propeller alphas set before CVehicle::Render");
    return kCVehicleRender;
}

std::string atomic_frame_name(GuestState& s, uint32_t atomic) {
    uint32_t name = guest_call<Conv::cdecl_, uint32_t>(s, kGetFrameNodeName, mem::read<uint32_t>(atomic + 4));
    return name ? mem::read_cstr(name, 32) : std::string();
}

uint32_t render_with_rotor_states(GuestState& s, uint32_t original, uint32_t atomic, bool blur) {
    uint32_t ref = render_state_get(s, kRsAlphaRef);
    uint32_t vertex_alpha = render_state_get(s, kRsVertexAlpha);
    if (blur) {
        render_state_set(s, kRsAlphaRef, 0);
        render_state_set(s, kRsVertexAlpha, 1);
    }
    uint32_t result = guest_call<Conv::cdecl_, uint32_t>(s, original, atomic);
    render_state_set(s, kRsAlphaRef, ref);
    render_state_set(s, kRsVertexAlpha, vertex_alpha);
    return result;
}

Memory* heli_rotor_alpha_render(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    log_once(reached, "rotors: helicopter rotor drawn with blending");
    eax(s) = render_with_rotor_states(s, original, stack_arg(s, 0), true);
    return_to_caller(s);
    return m;
}

Memory* plane_prop_alpha_render(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t atomic = stack_arg(s, 0);
    bool prop = atomic_frame_name(s, atomic).find("_prop") != std::string::npos;
    if (prop) {
        log_once(reached, "rotors: plane propeller drawn with blending");
    }
    eax(s) = render_with_rotor_states(s, original, atomic, prop);
    return_to_caller(s);
    return m;
}

void set_render_callback(uint32_t atomic, uint32_t callback) {
    mem::write<uint32_t>(atomic + 0x48, callback ? callback : kAtomicDefaultRender);
}

Memory* set_renderer_plane_or_big_vehicle(GuestState& s, uint32_t, Memory* m) {
    uint32_t original = redirect_original();
    uint32_t atomic = stack_arg(s, 0);
    uint32_t result = guest_call<Conv::cdecl_, uint32_t>(s, original, atomic, stack_arg(s, 1));
    if (atomic_frame_name(s, atomic).find("_prop") != std::string::npos) {
        static std::atomic<bool> reached = false;
        log_once(reached, "rotors: a _prop atomic gets the big vehicle alpha callback");
        set_render_callback(atomic, mem::read<uint32_t>(kBigVehicleAlphaCBOperand));
    }
    eax(s) = result;
    return_to_caller(s);
    return m;
}

Memory* set_renderer_heli_static_rotor(GuestState& s, uint32_t, Memory* m) {
    uint32_t original = redirect_original();
    uint32_t atomic = stack_arg(s, 0);
    uint32_t result = guest_call<Conv::cdecl_, uint32_t>(s, original, atomic, stack_arg(s, 1));
    std::string name = atomic_frame_name(s, atomic);
    if (name == "static_rotor" || name == "static_rotor2") {
        static std::atomic<bool> reached = false;
        log_once(reached, "rotors: static_rotor gets the rotor alpha callback");
    }
    if (name == "static_rotor") {
        set_render_callback(atomic, mem::read<uint32_t>(kHeliRotorAlphaCBOperand));
    } else if (name == "static_rotor2") {
        set_render_callback(atomic, mem::read<uint32_t>(kHeliTailRotorAlphaCBOperand));
    }
    eax(s) = result;
    return_to_caller(s);
    return m;
}

uint32_t double_rwheels_branch(GuestState& s) {
    static std::atomic<bool> reached = false;
    if (mem::read<uint32_t>(edi(s) + kModelVehicleType) == kVehicleTypeTrailer) {
        log_once(reached, "double rear wheels: a trailer gets its extra wheels");
        return 0x004c9235;
    }
    uint32_t wheel = eax(s);
    return wheel == 2 || wheel == 5 ? 0x004c9333 : 0x004c9235;
}

uint32_t trailer_double_rwheels(GuestState& s, uint32_t) { return double_rwheels_branch(s); }

uint32_t trailer_double_rwheels_mirrored(GuestState& s, uint32_t) {
    esp(s) += 0x18;
    eax(s) = mem::read<uint32_t>(esi(s) + mem::read<uint32_t>(ebx(s)) + 4);
    return double_rwheels_branch(s);
}

int32_t g_splash_for_last = 0;

uint32_t loading_screen_change(GuestState&, uint32_t) {
    static std::atomic<bool> reached = false;
    constexpr int32_t kNumSplashes = static_cast<int32_t>((kSplashesEnd - kSplashesBegin) / 4) - 1;
    auto curr = mem::read<int32_t>(kCurrDisplayedSplash);
    if (curr >= kNumSplashes) {
        mem::write<int32_t>(kCurrDisplayedSplash, 1);
        g_splash_for_last = kNumSplashes + 1;
        log_once(reached, "loading screens: the splash index loops back to the first screen");
    } else {
        mem::write<int32_t>(kCurrDisplayedSplash, curr + 1);
        g_splash_for_last = curr + 1;
    }
    return 0x00590ae4;
}

uint32_t loading_screen_last_splash(GuestState& s, uint32_t) {
    ecx(s) = static_cast<uint32_t>(g_splash_for_last);
    return 0x00590048;
}

Memory* ped_hit_passenger_line(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t vehicle = ecx(s);
    if (vehicle == guest_call<Conv::cdecl_, uint32_t>(s, kFindPlayerVehicle, 0xffffffffu, 0u)) {
        auto start = static_cast<uint32_t>(static_cast<double>(next_rand31()) / 0x7fffffff * 8.0);
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t passenger = mem::read<uint32_t>(vehicle + kVehiclePassengers + 4 * ((i + start) % 8));
            if (passenger) {
                log_once(reached, "speech: a passenger comments on the player running over a ped");
                if (ped_say(s, passenger, kContextCarHitPed) < 0) {
                    ped_say(s, passenger, kContextTaxiHitPed);
                }
                break;
            }
        }
    }
    eax(s) = guest_call<Conv::thiscall, uint32_t>(s, original, vehicle);
    return_to_caller(s);
    return m;
}

Memory* say_without_car_hit_ped(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    auto phrase = static_cast<uint16_t>(stack_arg(s, 0));
    if (phrase == kContextCarHitPed) {
        log_once(reached, "speech: pushing a ped with a car no longer plays CAR_HIT_PED");
        return_say(s, -1);
        return m;
    }
    return_say(s, say_original(s, ecx(s), phrase));
    return m;
}

struct TextIplLines {
    std::vector<std::string> pickups, car_generators, stunt_jumps;
};

TextIplLines& text_ipl_lines() {
    static TextIplLines lines;
    return lines;
}

std::string trim_spaces(std::string line) {
    size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos) {
        return line;
    }
    size_t last = line.find_last_not_of(' ');
    return line.substr(first, last - first + 1);
}

Memory* load_pickup_save_line(GuestState& s, uint32_t, Memory* m) {
    uint32_t line = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, redirect_original(), line);
    text_ipl_lines().pickups.push_back(trim_spaces(mem::read_cstr(line, 512)));
    return_to_caller(s);
    return m;
}

Memory* load_car_generator_save_line(GuestState& s, uint32_t, Memory* m) {
    uint32_t line = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, redirect_original(), line, stack_arg(s, 1));
    text_ipl_lines().car_generators.push_back(trim_spaces(mem::read_cstr(line, 512)));
    return_to_caller(s);
    return m;
}

Memory* load_stunt_jump_save_line(GuestState& s, uint32_t, Memory* m) {
    uint32_t line = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, redirect_original(), line);
    text_ipl_lines().stunt_jumps.push_back(trim_spaces(mem::read_cstr(line, 512)));
    return_to_caller(s);
    return m;
}

uint32_t guest_line(const std::string& text) {
    static const uint32_t buffer = allocator().alloc(512);
    size_t n = std::min<size_t>(text.size(), 511);
    std::memcpy(mem::ptr(buffer), text.data(), n);
    mem::write<uint8_t>(buffer + static_cast<uint32_t>(n), 0);
    return buffer;
}

Memory* reinit_game_object_variables(GuestState& s, uint32_t, Memory* m) {
    uint32_t original = redirect_original();
    for (uint32_t operand : kBoolResetOperands) {
        mem::write<uint8_t>(mem::read<uint32_t>(operand), 0);
    }
    for (uint32_t operand : kIntResetOperands) {
        mem::write<int32_t>(mem::read<uint32_t>(operand), 0);
    }
    float timer = (static_cast<float>(next_rand31()) / 2147483647.0f) * 600.0f + 600.0f;
    mem::write<float>(mem::read<uint32_t>(kMadDriverTimerOperand), timer);
    for (uint32_t operand : kTrueResetOperands) {
        mem::write<uint8_t>(mem::read<uint32_t>(operand), 1);
    }
    guest_call<Conv::cdecl_>(s, original);
    const auto& lines = text_ipl_lines();
    for (const auto& line : lines.pickups) {
        guest_call<Conv::cdecl_>(s, kLoadPickup, guest_line(line));
    }
    for (const auto& line : lines.car_generators) {
        guest_call<Conv::cdecl_>(s, kLoadCarGenerator, guest_line(line), 0u);
    }
    for (const auto& line : lines.stunt_jumps) {
        guest_call<Conv::cdecl_>(s, kLoadStuntJump, guest_line(line));
    }
    std::fprintf(stderr,
                 "[SP] new game: %zu bool, %zu counter and 3 other variables reset; %zu pickups, %zu car generators and %zu "
                 "stunt jumps reloaded from text IPLs\n",
                 std::size(kBoolResetOperands), std::size(kIntResetOperands), lines.pickups.size(), lines.car_generators.size(),
                 lines.stunt_jumps.size());
    return_to_caller(s);
    return m;
}

Memory* get_remap_index(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t vehicle = ecx(s);
    int32_t slot = mem::read<int16_t>(vehicle + kVehicleRemapTxd);
    if (slot == -1) {
        slot = mem::read<int16_t>(vehicle + kVehicleRemapTxdToLoad);
        if (slot != -1) {
            log_once(reached, "paintjobs: the remap index falls back to the pending remap");
        }
    }
    int32_t result = -1;
    if (slot != -1) {
        uint32_t info = mem::read<uint32_t>(kModelInfoPtrs + 4 * static_cast<uint32_t>(entity_model(vehicle)));
        for (uint32_t i = 0; i < 4; ++i) {
            int32_t id = mem::read<int16_t>(info + kModelRemapTxds + 2 * i);
            if (id == -1) {
                break;
            }
            if (id == slot) {
                result = static_cast<int32_t>(i);
                break;
            }
        }
    }
    eax(s) = static_cast<uint32_t>(result);
    return_to_caller(s);
    return m;
}

constexpr uint32_t kCurrZoneInfoOperand = 0x0058adb1;
constexpr uint32_t kHudColourOperand = 0x0058adf6;
constexpr uint32_t kHudColourZoneName = 3;
constexpr uint32_t kVectorToSunOperand = 0x006fc5ba;
constexpr uint32_t kCurrentStoredValueOperand = 0x006fc633;
constexpr uint32_t kFarClipOperand = 0x006fc5aa;
constexpr uint32_t kBigMessageXOperand = 0x0058c91b;
constexpr uint32_t kOddJob2XOffsetOperand = 0x0058d079;
constexpr uint32_t kHudMessagesWidthOperand = 0x0058d2dd;
constexpr uint32_t kInvincibilityKillPed = 0x004b3238;
constexpr uint32_t kInvincibilityDoNotKill = 0x004b339c;
constexpr uint32_t kWeaponLastType = 47;
constexpr uint32_t kWeaponUziDriveBy = 52;
constexpr float kSunFarClip = 600.0f;

uint32_t extra_directionals(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    bool more = eax(s) != 0 || static_cast<int32_t>(mem::read<uint32_t>(kFxQuality)) >= 1;
    ebx(s) = more ? 6 : 4;
    if (more) {
        log_once(reached, "lights: up to 6 extra directionals on Medium and higher Visual FX");
    }
    return 0x0073588f;
}

uint8_t blend_sqr(uint8_t from, uint8_t to, double t) {
    return static_cast<uint8_t>(std::sqrt((to * to) * t + (from * from) * (1.0 - t)));
}

Memory* zone_name_colour(GuestState& s, uint32_t, Memory* m) {
    static const bool coloured = mod_settings().flag("silentpatch.coloured_zone_names");
    static std::atomic<bool> reached = false;
    uint32_t rgba = ecx(s);
    auto r = static_cast<uint8_t>(stack_arg(s, 0));
    auto g = static_cast<uint8_t>(stack_arg(s, 1));
    auto b = static_cast<uint8_t>(stack_arg(s, 2));
    auto a = static_cast<uint8_t>(stack_arg(s, 3));
    uint32_t hud = mem::read<uint32_t>(kHudColourOperand) + 4 * kHudColourZoneName;
    std::array<uint8_t, 3> out = {mem::read<uint8_t>(hud), mem::read<uint8_t>(hud + 1), mem::read<uint8_t>(hud + 2)};
    if (coloured) {
        uint32_t zone = mem::read<uint32_t>(mem::read<uint32_t>(kCurrZoneInfoOperand));
        double t = std::min(static_cast<double>(mem::read<uint8_t>(zone + 0x0e)) / 120.0, 1.0);
        out = {blend_sqr(out[0], r, t), blend_sqr(out[1], g, t), blend_sqr(out[2], b, t)};
        log_once(reached, "zone names: the zone name takes the owning gang's colour");
    }
    mem::write<uint8_t>(rgba, out[0]);
    mem::write<uint8_t>(rgba + 1, out[1]);
    mem::write<uint8_t>(rgba + 2, out[2]);
    mem::write<uint8_t>(rgba + 3, a);
    eax(s) = rgba;
    return_to_caller(s, 16);
    return m;
}

uint32_t directional_from_sun(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    uint32_t vectors = mem::read<uint32_t>(kVectorToSunOperand);
    uint32_t index = mem::read<uint32_t>(mem::read<uint32_t>(kCurrentStoredValueOperand));
    uint32_t v = vectors + 12 * index;
    ecx(s) = mem::read<uint32_t>(v);
    edx(s) = mem::read<uint32_t>(v + 4);
    eax(s) = mem::read<uint32_t>(v + 8);
    log_once(reached, "lights: the directional light follows the sun");
    return 0x00735629;
}

uint32_t sun_size_far_clip(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    float far_clip = mem::read<float>(mem::read<uint32_t>(kFarClipOperand));
    x87_push(s, std::min(far_clip, kSunFarClip));
    log_once(reached, "sun: drawn at a far clip of at most 600");
    return 0x006fc5ae;
}

uint32_t minimal_hud_mode(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    ecx(s) = 0;
    log_once(reached, "minimal HUD: enabled in CHud::ReInitialise");
    return 0x0058890a;
}

Memory* hud_rgba_float_alpha(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t rgba = ecx(s);
    mem::write<uint8_t>(rgba, static_cast<uint8_t>(stack_arg(s, 0)));
    mem::write<uint8_t>(rgba + 1, static_cast<uint8_t>(stack_arg(s, 1)));
    mem::write<uint8_t>(rgba + 2, static_cast<uint8_t>(stack_arg(s, 2)));
    mem::write<uint8_t>(rgba + 3, static_cast<uint8_t>(std::bit_cast<float>(stack_arg(s, 3))));
    log_once(reached, "HUD: the fading item keeps its float alpha");
    eax(s) = rgba;
    return_to_caller(s, 16);
    return m;
}

Memory* xlu_sprite_float_alpha(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t alpha = float_arg(s, 10);
    mem::write<uint32_t>(alpha, static_cast<uint8_t>(mem::read<float>(alpha)));
    log_once(reached, "HUD: the radar sprite keeps its float alpha");
    eip(s) = redirect_original();
    return m;
}

uint32_t true_invincibility(GuestState& s, uint32_t) {
    static std::atomic<bool> reached = false;
    uint32_t weapon = mem::read<uint32_t>(ebp(s) + 0x0c);
    if (weapon < kWeaponLastType) {
        return kInvincibilityDoNotKill;
    }
    if (weapon == kWeaponUziDriveBy) {
        log_once(reached, "invincibility: police helicopter bullets don't hurt CJ");
        return kInvincibilityDoNotKill;
    }
    return kInvincibilityKillPed;
}

Memory* sliding_mission_title(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    float x = mem::read<float>(mem::read<uint32_t>(kBigMessageXOperand) + 4);
    mem::write<float>(float_arg(s, 0), x * screen_width() / 640.0f);
    log_once(reached, "sliding texts: the mission title slides in");
    eip(s) = redirect_original();
    return m;
}

Memory* sliding_mission_title_wrap(GuestState& s, uint32_t, Memory* m) {
    mem::write<float>(float_arg(s, 0), -500.0f * screen_width() * operand_value(kHudMessagesWidthOperand));
    eip(s) = redirect_original();
    return m;
}

Memory* sliding_odd_job_text(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    float offset = mem::read<float>(mem::read<uint32_t>(kOddJob2XOffsetOperand));
    uint32_t x = float_arg(s, 0);
    mem::write<float>(x, mem::read<float>(x) - offset * screen_width() / 640.0f);
    log_once(reached, "sliding texts: the odd job text slides in");
    eip(s) = redirect_original();
    return m;
}

constexpr unsigned char kLunarPng[] = {
#include "lunar64_png.inc"
};
constexpr uint32_t kRwRasterLock = 0x007fb2d0;
constexpr uint32_t kRwRasterUnlock = 0x007faec0;
constexpr uint32_t kRwD3D9SetRenderState = 0x007fc2d0;
constexpr uint32_t kRpAnimBlendClumpGetAssociation = 0x004d68b0;
constexpr uint32_t kRsTextureRaster = 1;
constexpr uint32_t kRsSrcBlend = 10;
constexpr uint32_t kRsDestBlend = 11;
constexpr uint32_t kRsAlphaTestFunction = 29;
constexpr uint32_t kAlphaTestAlways = 8;
constexpr uint32_t kBlendZero = 1;
constexpr uint32_t kBlendSrcAlpha = 5;
constexpr uint32_t kD3DRSColorWriteEnable = 168;
constexpr uint32_t kColorWriteAlpha = 0x8;
constexpr uint32_t kColorWriteAll = 0xf;
constexpr uint32_t kRasterTexture8888 = 0x0504;
constexpr uint32_t kRasterLockWrite = 1;
constexpr uint32_t kSkinPluginId = 0x116;
constexpr uint32_t kAnimStdCarSitKart = 95;
constexpr uint32_t kAnimStdBoatDrive = 81;
constexpr uint32_t kAnimFlagPartial = 0x10;
constexpr uint16_t kSequenceUsesBoneTag = 0x10;
constexpr int16_t kRadioAnimBones[] = {5, 21, 22, 23, 24, 25, 26, 301};
constexpr uint32_t kTaskComplexSequence = 244;
constexpr uint32_t kTaskSimpleGangDriveBy = 1022;
constexpr uint32_t kVehicleDriver = 0x460;
constexpr uint32_t kCarWheelStatus = 0x5a5;
constexpr uint32_t kWheelDetached = 2;
constexpr uint32_t kGeometryModulateMaterialColor = 0x40;

std::vector<uint8_t> decode_png_alpha(const unsigned char* data, size_t size, uint32_t& width, uint32_t& height) {
    std::vector<uint8_t> alpha;
    CFDataRef bytes = CFDataCreate(nullptr, data, static_cast<CFIndex>(size));
    CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
    CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    if (image) {
        width = static_cast<uint32_t>(CGImageGetWidth(image));
        height = static_cast<uint32_t>(CGImageGetHeight(image));
        std::vector<uint8_t> rgba(4 * width * height);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 8, 4 * width, space,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        if (context) {
            CGContextSetBlendMode(context, kCGBlendModeCopy);
            CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
            alpha.resize(width * height);
            for (uint32_t i = 0; i < width * height; ++i) {
                alpha[i] = rgba[4 * i + 3];
            }
            CGContextRelease(context);
        }
        CGColorSpaceRelease(space);
        CGImageRelease(image);
    }
    if (source) {
        CFRelease(source);
    }
    if (bytes) {
        CFRelease(bytes);
    }
    return alpha;
}

uint32_t moon_mask_raster(GuestState& s) {
    static uint32_t raster = [&] {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> alpha = decode_png_alpha(kLunarPng, sizeof(kLunarPng), width, height);
        if (alpha.empty()) {
            std::fprintf(stderr, "[SP] moon phases: the moon mask did not decode\n");
            return 0u;
        }
        uint32_t r = guest_call<Conv::cdecl_, uint32_t>(s, kRwRasterCreate, width, height, 32u, kRasterTexture8888);
        uint32_t pixels = r ? guest_call<Conv::cdecl_, uint32_t>(s, kRwRasterLock, r, 0u, kRasterLockWrite) : 0;
        if (!pixels) {
            std::fprintf(stderr, "[SP] moon phases: the moon mask raster could not be created\n");
            return 0u;
        }
        uint32_t stride = mem::read<uint32_t>(r + 0x18);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                mem::write<uint32_t>(pixels + y * stride + 4 * x, static_cast<uint32_t>(alpha[y * width + x]) << 24);
            }
        }
        guest_call<Conv::cdecl_, uint32_t>(s, kRwRasterUnlock, r);
        std::fprintf(stderr, "[SP] moon phases: %ux%u moon mask raster created\n", width, height);
        return r;
    }();
    return raster;
}

Memory* moon_phases_mask(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    std::array<uint32_t, 13> args{};
    for (unsigned i = 0; i < args.size(); ++i) {
        args[i] = stack_arg(s, i);
    }
    uint32_t raster = moon_mask_raster(s);
    uint32_t alpha_test = render_state_get(s, kRsAlphaTestFunction);
    if (raster) {
        render_state_set(s, kRsTextureRaster, raster);
    }
    render_state_set(s, kRsAlphaTestFunction, kAlphaTestAlways);
    render_state_set(s, kRsSrcBlend, kBlendSrcAlpha);
    render_state_set(s, kRsDestBlend, kBlendZero);
    guest_call<Conv::cdecl_>(s, kRwD3D9SetRenderState, kD3DRSColorWriteEnable, kColorWriteAlpha);
    call_guest(s, original, std::span<const uint32_t>(args.data(), args.size()), false);
    guest_call<Conv::cdecl_>(s, kRwD3D9SetRenderState, kD3DRSColorWriteEnable, kColorWriteAll);
    render_state_set(s, kRsAlphaTestFunction, alpha_test);
    log_once(reached, "moon phases: the moon is masked by its phase");
    return_to_caller(s);
    return m;
}

Memory* building_pipeline_skip_skinned(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t atomic = stack_arg(s, 0);
    uint32_t pipeline = mem::read<uint32_t>(atomic + 0x6c);
    if (pipeline && mem::read<uint32_t>(pipeline + 0x2c) == kSkinPluginId) {
        log_once(reached, "pipelines: a skinned atomic keeps its skin pipeline");
        eax(s) = atomic;
        return_to_caller(s);
        return m;
    }
    eip(s) = redirect_original();
    return m;
}

Memory* radio_change_anim(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t clump = stack_arg(s, 0);
    if (guest_call<Conv::cdecl_, uint32_t>(s, kRpAnimBlendClumpGetAssociation, clump, kAnimStdCarSitKart)) {
        eax(s) = 0;
        return_to_caller(s);
        return m;
    }
    bool boat = guest_call<Conv::cdecl_, uint32_t>(s, kRpAnimBlendClumpGetAssociation, clump, kAnimStdBoatDrive) != 0;
    uint32_t assoc = guest_call<Conv::cdecl_, uint32_t>(s, original, clump, stack_arg(s, 1), stack_arg(s, 2), stack_arg(s, 3));
    if (assoc) {
        int32_t count = mem::read<int16_t>(assoc + 0x0c);
        uint32_t nodes = mem::read<uint32_t>(assoc + 0x10);
        for (int32_t i = 0; i < count; ++i) {
            uint32_t node = nodes + 0x18 * static_cast<uint32_t>(i);
            uint32_t sequence = mem::read<uint32_t>(node + 0x10);
            if (!sequence || !(mem::read<uint16_t>(sequence + 4) & kSequenceUsesBoneTag)) {
                continue;
            }
            int16_t bone = mem::read<int16_t>(sequence);
            if (std::ranges::find(kRadioAnimBones, bone) == std::end(kRadioAnimBones)) {
                mem::write<uint32_t>(node + 0x10, 0);
            }
        }
        if (boat) {
            mem::write<uint16_t>(assoc + 0x2e, mem::read<uint16_t>(assoc + 0x2e) & ~kAnimFlagPartial);
        }
        log_once(reached, "radio: the station change animation moves only the right arm and head");
    }
    eax(s) = assoc;
    return_to_caller(s);
    return m;
}

uint32_t task_type(GuestState& s, uint32_t task) {
    return guest_call<Conv::thiscall, uint32_t>(s, mem::read<uint32_t>(mem::read<uint32_t>(task) + 0x10), task);
}

Memory* biker_cop_abort_drive_by(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t vehicle = stack_arg(s, 0);
    guest_call<Conv::cdecl_>(s, redirect_original(), vehicle);
    uint32_t driver = mem::read<uint32_t>(vehicle + kVehicleDriver);
    uint32_t intelligence = driver ? mem::read<uint32_t>(driver + kPedIntelligence) : 0;
    uint32_t primary = intelligence ? mem::read<uint32_t>(intelligence + 0x10) : 0;
    if (primary && task_type(s, primary) == kTaskComplexSequence) {
        bool drive_by = false;
        for (uint32_t i = 0; i < 8 && !drive_by; ++i) {
            uint32_t task = mem::read<uint32_t>(primary + 0x10 + 4 * i);
            drive_by = task && task_type(s, task) == kTaskSimpleGangDriveBy;
        }
        if (drive_by) {
            guest_call<Conv::thiscall, uint8_t>(s, mem::read<uint32_t>(mem::read<uint32_t>(primary) + 0x18), primary, driver, 1u, 0u);
            log_once(reached, "biker cops: the drive-by is aborted when the wanted level is lost");
        }
    }
    return_to_caller(s);
    return m;
}

Memory* checkpoint_modulate_colour(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    for_each_clump_atomic(stack_arg(s, 0), [](uint32_t atomic) {
        if (uint32_t geometry = mem::read<uint32_t>(atomic + 0x18)) {
            mem::write<uint32_t>(geometry + 8, mem::read<uint32_t>(geometry + 8) | kGeometryModulateMaterialColor);
        }
    });
    log_once(reached, "checkpoints: racing checkpoints modulate their material colour");
    eip(s) = redirect_original();
    return m;
}

bool frame_has_rendered_object(uint32_t frame) {
    uint32_t head = frame + 0x90;
    for (uint32_t link = mem::read<uint32_t>(head); link != head; link = mem::read<uint32_t>(link)) {
        if (mem::read<uint8_t>(link - 8 + 2) & kAtomicRender) {
            return true;
        }
    }
    return false;
}

void hide_frame_objects(uint32_t frame) {
    uint32_t head = frame + 0x90;
    for (uint32_t link = mem::read<uint32_t>(head); link != head; link = mem::read<uint32_t>(link)) {
        mem::write<uint8_t>(link - 8 + 2, 0);
    }
}

Memory* spawn_flying_wheels(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t original = redirect_original();
    uint32_t car = ecx(s);
    uint32_t model = stack_arg(s, 1);
    unsigned spawned = 0;
    auto try_spawn = [&](uint32_t node) {
        uint32_t frame = car_node(car, node);
        if (frame && frame_has_rendered_object(frame)) {
            guest_call<Conv::thiscall>(s, original, car, node, model);
            hide_frame_objects(frame);
            ++spawned;
        }
    };
    auto lost = [&](uint32_t wheel) { return mem::read<uint8_t>(car + kCarWheelStatus + wheel) == kWheelDetached; };
    if (lost(0)) {
        try_spawn(5);
    }
    if (lost(2)) {
        try_spawn(2);
    }
    if (lost(1)) {
        try_spawn(6);
        try_spawn(7);
    }
    if (lost(3)) {
        try_spawn(3);
        try_spawn(4);
    }
    if (spawned && !reached.exchange(true)) {
        std::fprintf(stderr, "[SP] exploding cars: %u wheel(s) spawned from the damaged wheels (model %d)\n", spawned,
                     entity_model(car));
    }
    return_to_caller(s, 8);
    return m;
}

float exploding_wheel_randomness(float) { return -4.0f; }

constexpr uint32_t kModelPlateMaterial = 0x24;
constexpr uint32_t kModelColourMaterials = 0x60;
constexpr uint32_t kStoredCarBombType = 0x39;
constexpr uint32_t kVehicleBombOnBoard = 0x4a8;
constexpr uint32_t kVehicleBombOwner = 0x4cc;
constexpr uint32_t kVehicleClass = 0x590;
constexpr uint32_t kVehicleClassAutomobile = 0;
constexpr uint32_t kVehicleClassBike = 9;

Memory* vehicle_model_info_init(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    static const uint32_t dummy = allocator().alloc(4);
    uint32_t info = ecx(s);
    eax(s) = guest_call<Conv::thiscall, uint32_t>(s, redirect_original(), info);
    mem::write<uint32_t>(info + kModelPlateMaterial, dummy);
    for (uint32_t o = kModelColourMaterials; o < kStaticDirtMaterials + 4 * kInPlaceDirt; o += 4) {
        mem::write<uint32_t>(info + o, 0);
    }
    mem::write<uint32_t>(info + kDirtMaterialsPtr, 0);
    mem::write<uint32_t>(info + kNumDirtMaterials, 0);
    log_once(reached, "model info: CVehicleModelInfo's material lists start empty");
    return_to_caller(s);
    return m;
}

Memory* restore_car_bomb_owner(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    uint32_t stored = ecx(s);
    uint32_t vehicle = guest_call<Conv::thiscall, uint32_t>(s, redirect_original(), stored);
    uint8_t bomb = mem::read<uint8_t>(stored + kStoredCarBombType);
    if (vehicle && bomb) {
        uint32_t cls = mem::read<uint32_t>(vehicle + kVehicleClass);
        if (cls == kVehicleClassAutomobile || cls == kVehicleClassBike) {
            uint8_t flags = mem::read<uint8_t>(vehicle + kVehicleBombOnBoard);
            mem::write<uint8_t>(vehicle + kVehicleBombOnBoard, static_cast<uint8_t>((flags & ~7u) | (bomb & 7u)));
            mem::write<uint32_t>(vehicle + kVehicleBombOwner, guest_call<Conv::cdecl_, uint32_t>(s, kFindPlayerPed, 0xffffffffu));
            log_once(reached, "garages: a stored car's bomb is restored with CJ as its owner");
        }
    }
    eax(s) = vehicle;
    return_to_caller(s);
    return m;
}

Memory* handling_name_keep_pointer(GuestState& s, uint32_t, Memory* m) {
    mem::write<uint32_t>(stack_arg(s, 0), stack_arg(s, 1));
    eax(s) = stack_arg(s, 0);
    return_to_caller(s);
    return m;
}

Memory* handling_name_compare(GuestState& s, uint32_t, Memory* m) {
    static std::atomic<bool> reached = false;
    auto* a = reinterpret_cast<const char*>(mem::ptr(stack_arg(s, 0)));
    auto* b = reinterpret_cast<const char*>(mem::ptr(mem::read<uint32_t>(stack_arg(s, 1))));
    eax(s) = static_cast<uint32_t>(std::strcmp(a, b));
    log_once(reached, "handling: names are matched whole");
    return_to_caller(s);
    return m;
}

constexpr NativeSymbol kSymbols[] = {
    {"sp_is_metric", native_trace<Conv::cdecl_, is_metric>},
    {"sp_ascii_to_gxt_null_check", ascii_to_gxt_null_check},
    {"sp_coldata_malloc_init", coldata_malloc_init},
    {"sp_coldata_new_init", coldata_new_init},
    {"sp_escalator_switch_off", escalator_switch_off},
    {"sp_try_to_free_up_temp_objects", try_to_free_up_temp_objects},
    {"sp_restore_replay_rebuild_player", restore_replay_rebuild_player},
    {"sp_rebuild_player_load_motion_groups", rebuild_player_load_motion_groups},
    {"sp_vehicles_ide_defaults", vehicles_ide_defaults},
    {"sp_automobile_ctor_or_train", automobile_ctor_or_train},
    {"sp_timer_ftol", timer_ftol},
    {"sp_zone_locked_or_unlocked", native_trace<Conv::cdecl_, zone_locked_or_unlocked>},
    {"sp_scripts_load_basketball_fix", scripts_load_basketball_fix},
    {"sp_start_mission_script_fixes", start_mission_script_fixes},
    {"sp_corona_flare_scale", corona_flare_scale},
    {"sp_anim_set_current_anim", anim_set_current_anim},
    {"sp_record_vehicle_deleted", record_vehicle_deleted},
    {"sp_interesting_and_impoundable", interesting_and_impoundable},
    {"sp_timecyc_line", timecyc_line},
    {"sp_script_sprites_linear", script_sprites_linear},
    {"sp_rand31", rand31},
    {"sp_create_mirror_buffers", create_mirror_buffers},
    {"sp_audio_utility_time_ms", native_trace<Conv::cdecl_, audio_utility_time_ms>},
    {"sp_set_position_and_area_code", set_position_and_area_code},
    {"sp_find_player_entity_with_rc", native_trace<Conv::cdecl_, find_player_entity_with_rc>},
    {"sp_find_player_vehicle_with_rc", find_player_vehicle_with_rc},
    {"sp_give_weapon_min_ammo", give_weapon_min_ammo},
    {"sp_bicycle_fire_noop", bicycle_fire_noop},
    {"sp_start_fire_null_check", start_fire_null_check},
    {"sp_corona_rotate_aspect", corona_rotate_aspect},
    {"sp_render_shadows_alpha_always", render_shadows_alpha_always},
    {"sp_weapon_info_or_camera", weapon_info_or_camera},
    {"sp_prepare_vehicle_for_ped_exit", prepare_vehicle_for_ped_exit},
    {"sp_rand16", rand16},
    {"sp_shooting_stars_untextured", shooting_stars_untextured},
    {"sp_plane_sector_numbers", plane_sector_numbers},
    {"sp_credits_set_scale", credits_set_scale},
    {"sp_credits_print", credits_print},
    {"sp_credits_print_shadow", credits_print_shadow},
    {"sp_underwater_ripple_frequency", underwater_ripple_frequency},
    {"sp_crosshair_sprite_scale", crosshair_sprite_scale},
    {"sp_map_limit_scaled", map_limit_scaled},
    {"sp_wrap_full_width", wrap_full_width},
    {"sp_wrap_menu_left", wrap_menu_left},
    {"sp_wrap_menu_right", wrap_menu_right},
    {"sp_wrap_replay_right", wrap_replay_right},
    {"sp_jet_matrix_flags", jet_matrix_flags},
    {"sp_hunter_door_unmark", hunter_door_unmark},
    {"sp_say_weather_reply", say_weather_reply},
    {"sp_sound_bank_weather_fallback", sound_bank_weather_fallback},
    {"sp_cj_mood_override", cj_mood_override},
    {"sp_get_voice_typos", get_voice_typos},
    {"sp_peds_ide_voice_names", peds_ide_voice_names},
    {"sp_turf_text_label_stricmp", turf_text_label_stricmp},
    {"sp_cj_mood_store", cj_mood_store},
    {"sp_say_solicit_then_mood", say_solicit_then_mood},
    {"sp_gang_abuse_lsv", gang_abuse_lsv},
    {"sp_gang_attack_player", gang_attack_player},
    {"sp_say_crash_generic_fallback", say_crash_generic_fallback},
    {"sp_arrest_cj_chased_reply", arrest_cj_chased_reply},
    {"sp_shoot_context_check_target", shoot_context_check_target},
    {"sp_peds_group_for_cops", peds_group_for_cops},
    {"sp_dismiss_reply_where_you_from", dismiss_reply_where_you_from},
    {"sp_drugged_ignore_on_homie", drugged_ignore_on_homie},
    {"sp_good_citizen_text", good_citizen_text},
    {"sp_radio_name_centre_size", radio_name_centre_size},
    {"sp_fort_carson_beagle", fort_carson_beagle},
    {"sp_automobile_pre_render", automobile_pre_render},
    {"sp_plane_pre_render", plane_pre_render},
    {"sp_trailer_tow_bar_pos", trailer_tow_bar_pos},
    {"sp_hierarchy_typos", hierarchy_typos},
    {"sp_lit_flying_component", lit_flying_component},
    {"sp_remap_dirt", remap_dirt},
    {"sp_vehicle_model_shutdown", vehicle_model_shutdown},
    {"sp_set_car_custom_plate", set_car_custom_plate},
    {"sp_plate_texture_create", plate_texture_create},
    {"sp_plate_before_render", plate_before_render},
    {"sp_plate_after_render", plate_after_render},
    {"sp_plate_setup_after_upgrade", plate_setup_after_upgrade},
    {"sp_dark_vehicle_check", dark_vehicle_check},
    {"sp_render_weapon_with_ped", render_weapon_with_ped},
    {"sp_render_weapon_peds_for_pc", render_weapon_peds_for_pc},
    {"sp_shadow_camera_update", shadow_camera_update},
    {"sp_heli_render_rotors", heli_render_rotors},
    {"sp_heli_rotor_alpha_render", heli_rotor_alpha_render},
    {"sp_plane_prop_alpha_render", plane_prop_alpha_render},
    {"sp_set_renderer_plane_or_big_vehicle", set_renderer_plane_or_big_vehicle},
    {"sp_set_renderer_heli_static_rotor", set_renderer_heli_static_rotor},
    {"sp_ped_hit_passenger_line", ped_hit_passenger_line},
    {"sp_say_without_car_hit_ped", say_without_car_hit_ped},
    {"sp_load_pickup_save_line", load_pickup_save_line},
    {"sp_load_car_generator_save_line", load_car_generator_save_line},
    {"sp_load_stunt_jump_save_line", load_stunt_jump_save_line},
    {"sp_reinit_game_object_variables", reinit_game_object_variables},
    {"sp_get_remap_index", get_remap_index},
    {"sp_zone_name_colour", zone_name_colour},
    {"sp_hud_rgba_float_alpha", hud_rgba_float_alpha},
    {"sp_xlu_sprite_float_alpha", xlu_sprite_float_alpha},
    {"sp_sliding_mission_title", sliding_mission_title},
    {"sp_sliding_mission_title_wrap", sliding_mission_title_wrap},
    {"sp_sliding_odd_job_text", sliding_odd_job_text},
    {"sp_moon_phases_mask", moon_phases_mask},
    {"sp_building_pipeline_skip_skinned", building_pipeline_skip_skinned},
    {"sp_radio_change_anim", radio_change_anim},
    {"sp_biker_cop_abort_drive_by", biker_cop_abort_drive_by},
    {"sp_checkpoint_modulate_colour", checkpoint_modulate_colour},
    {"sp_spawn_flying_wheels", spawn_flying_wheels},
    {"sp_vehicle_model_info_init", vehicle_model_info_init},
    {"sp_restore_car_bomb_owner", restore_car_bomb_owner},
    {"sp_handling_name_keep_pointer", handling_name_keep_pointer},
    {"sp_handling_name_compare", handling_name_compare},
};

constexpr MidHookSymbol kMidHooks[] = {
    {"sp_jetpack_keyboard_hover", jetpack_keyboard_hover},
    {"sp_escalators_update", escalators_update},
    {"sp_grab_screen_release_surface", grab_screen_release_surface},
    {"sp_player_ped_data_assignment", player_ped_data_assignment},
    {"sp_cdstream_sync", cdstream_sync},
    {"sp_cdstream_thread_done", cdstream_thread_done},
    {"sp_gym_glitch_get_date", gym_glitch_get_date},
    {"sp_frame_pacing", frame_pacing},
    {"sp_lens_flush_before_ztest", lens_flush_before_ztest},
    {"sp_lens_init_after_ztest", lens_init_after_ztest},
    {"sp_car_damage_component", car_damage_component},
    {"sp_weapon_range_vehicle_check", weapon_range_vehicle_check},
    {"sp_stencil_shadow_pads", stencil_shadow_pads},
    {"sp_bicycle_fire_driver", bicycle_fire_driver},
    {"sp_reset_comps_for_no_extras", reset_comps_for_no_extras},
    {"sp_follow_car_mouse_forward", follow_car_mouse_forward},
    {"sp_open_top_or_quad", open_top_or_quad},
    {"sp_quad_handlebar_interp", quad_handlebar_interp},
    {"sp_biker_cop_model", biker_cop_model},
    {"sp_remove_driver_status", remove_driver_status},
    {"sp_riot_skip_player_group", riot_skip_player_group},
    {"sp_credits_fixed_height", credits_fixed_height},
    {"sp_credits_end_fixed_height", credits_end_fixed_height},
    {"sp_big_message_fixed_width", big_message_fixed_width},
    {"sp_video_mode_post_fx", video_mode_post_fx},
    {"sp_nitrous_reverse", nitrous_reverse},
    {"sp_gang_war_friendly_blips", gang_war_friendly_blips},
    {"sp_speech_lookup_tables", speech_lookup_tables},
    {"sp_pain_sprayed", pain_sprayed},
    {"sp_shoot_store_target", shoot_store_target},
    {"sp_good_citizen_check", good_citizen_check},
    {"sp_shadow_point_transform", shadow_point_transform},
    {"sp_firela_hydraulics_test", firela_hydraulics_test},
    {"sp_firela_camera_control", firela_camera_control},
    {"sp_tug_tow_bar_frame", tug_tow_bar_frame},
    {"sp_detached_part_render", detached_part_render},
    {"sp_find_editable_material_list", find_editable_material_list},
    {"sp_road_reflection_depth", road_reflection_depth},
    {"sp_light_materials_save", light_materials_save},
    {"sp_dark_vehicle_branch_a", dark_vehicle_branch_a},
    {"sp_dark_vehicle_branch_b", dark_vehicle_branch_b},
    {"sp_dark_vehicle_branch_c", dark_vehicle_branch_c},
    {"sp_lightbeam_push_state", lightbeam_push_state},
    {"sp_lightbeam_pop_state", lightbeam_pop_state},
    {"sp_automobile_fix", automobile_fix},
    {"sp_plane_fix", plane_fix},
    {"sp_plane_render_rotors", plane_render_rotors},
    {"sp_trailer_double_rwheels", trailer_double_rwheels},
    {"sp_trailer_double_rwheels_mirrored", trailer_double_rwheels_mirrored},
    {"sp_loading_screen_change", loading_screen_change},
    {"sp_loading_screen_last_splash", loading_screen_last_splash},
    {"sp_extra_directionals", extra_directionals},
    {"sp_directional_from_sun", directional_from_sun},
    {"sp_sun_size_far_clip", sun_size_far_clip},
    {"sp_minimal_hud_mode", minimal_hud_mode},
    {"sp_true_invincibility", true_invincibility},
};

constexpr SlotSymbol kSlots[] = {
    {"sp_meters_to_feet", meters_to_feet},
    {"sp_light_status_randomness", light_status_randomness},
    {"sp_credits_top_margin", credits_top_margin},
    {"sp_credits_bottom_margin", credits_bottom_margin},
    {"sp_crosshair_triangle", crosshair_triangle},
    {"sp_map_cursor_x", map_cursor_x},
    {"sp_map_cursor_y", map_cursor_y},
    {"sp_text_padding_x", text_padding_x},
    {"sp_text_padding_y", text_padding_y},
    {"sp_engine_comp_speed", engine_comp_speed},
    {"sp_exploding_wheel_randomness", exploding_wheel_randomness},
};

}

std::span<const NativeSymbol> silentpatch_symbols() { return kSymbols; }

std::span<const MidHookSymbol> silentpatch_mid_hooks() { return kMidHooks; }

std::span<const SlotSymbol> silentpatch_slots() { return kSlots; }

}
