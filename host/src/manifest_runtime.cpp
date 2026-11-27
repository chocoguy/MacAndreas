#include "manifest_runtime.hpp"

#include "dispatch.hpp"
#include "guest_memory.hpp"
#include "mod_settings.hpp"
#include "native/natives.hpp"

#include <cstdio>
#include <cstring>
#include <format>
#include <vector>

namespace ma::host {

namespace {

std::expected<bool, std::string> enabled(const manifest::Entry& e) {
    if (e.setting.empty()) {
        return true;
    }
    const auto* spec = mod_schema().find(e.setting);
    if (!spec) {
        return std::unexpected(std::format("setting \"{}\" is not in the settings schema", e.setting));
    }
    if (spec->type != settings::Type::boolean) {
        return std::unexpected(std::format("setting \"{}\" gates an entry, so it must be a bool", e.setting));
    }
    return mod_settings().flag(e.setting);
}

struct OperandSlot {
    uint32_t slot;
    uint32_t original;
    float initial;
    native::SlotFn fn;
};

std::vector<OperandSlot>& operand_slots() {
    static std::vector<OperandSlot> slots;
    return slots;
}

std::expected<uint32_t, std::string> call_target(uint32_t site, uint8_t opcode, uint8_t modrm) {
    if (opcode == 0xE8) {
        return site + 5 + mem::read<uint32_t>(site + 1);
    }
    if (opcode == 0xFF && modrm == 0x15) {
        return mem::read<uint32_t>(mem::read<uint32_t>(site + 2));
    }
    return std::unexpected(std::format("0x{:08x} is not a call rel32 or call [mem]", site));
}

}

std::expected<ManifestStats, std::string> apply_runtime_manifest(const manifest::Manifest& m) {
    ManifestStats stats;
    std::string errors;
    auto fail = [&](const manifest::Entry& e, const std::string& why) {
        errors += std::format("{}:{}: {}\n", e.file, e.line, why);
    };
    for (const auto& e : m.entries) {
        auto on = enabled(e);
        if (!on) {
            fail(e, on.error());
            continue;
        }
        switch (e.kind) {
        case manifest::Kind::func_replace: {
            TraceFn fn = native::find_symbol(e.target);
            if (!fn) {
                fail(e, std::format("unknown native symbol \"{}\"", e.target));
                break;
            }
            if (!*on) {
                ++stats.disabled;
                break;
            }
            register_trace(e.addr, fn);
            std::fprintf(stderr, "[MANIFEST] func_replace 0x%08x -> %s (%s)\n", e.addr, e.target.c_str(),
                         e.group.c_str());
            ++stats.replaced;
            break;
        }
        case manifest::Kind::call_redirect: {
            TraceFn fn = native::find_symbol(e.target);
            if (!fn) {
                fail(e, std::format("unknown native symbol \"{}\"", e.target));
                break;
            }
            uint8_t opcode = mem::read<uint8_t>(e.addr);
            auto original = call_target(e.addr, opcode, mem::read<uint8_t>(e.addr + 1));
            if (!original) {
                fail(e, original.error());
                break;
            }
            uint32_t thunk = manifest::redirect_address(e.addr);
            mem::write<uint8_t>(e.addr, 0xE8);
            mem::write<uint32_t>(e.addr + 1, thunk - (e.addr + 5));
            if (opcode == 0xFF) {
                mem::write<uint8_t>(e.addr + 5, 0x90);
            }
            register_redirect(e.addr, *original, *on ? fn : nullptr);
            std::fprintf(stderr, "[MANIFEST] call_redirect 0x%08x -> %s%s (%s)\n", e.addr, e.target.c_str(),
                         *on ? "" : " [off]", e.group.c_str());
            ++(*on ? stats.redirected : stats.disabled);
            break;
        }
        case manifest::Kind::mid_hook: {
            MidHookFn fn = native::find_mid_hook(e.target);
            if (!fn) {
                fail(e, std::format("unknown native mid hook \"{}\"", e.target));
                break;
            }
            if (!*on) {
                ++stats.disabled;
                break;
            }
            register_mid_hook(e.addr, fn);
            std::fprintf(stderr, "[MANIFEST] mid_hook 0x%08x -> %s (%s)\n", e.addr, e.target.c_str(), e.group.c_str());
            ++stats.hooked;
            break;
        }
        case manifest::Kind::operand_slot: {
            native::SlotFn fn = native::find_slot(e.target);
            if (!fn) {
                fail(e, std::format("unknown native slot \"{}\"", e.target));
                break;
            }
            if (!manifest::is_operand_slot_instruction(mem::read<uint8_t>(e.addr), mem::read<uint8_t>(e.addr + 1))) {
                fail(e, std::format("0x{:08x} is not an x87 load or arithmetic from an absolute m32 operand", e.addr));
                break;
            }
            uint32_t slot = manifest::operand_slot_address(e.addr);
            uint32_t original = mem::read<uint32_t>(e.addr + 2);
            mem::write<uint32_t>(e.addr + 2, slot);
            operand_slots().push_back({slot, original, mem::read<float>(original), *on ? fn : nullptr});
            ++(*on ? stats.slotted : stats.disabled);
            break;
        }
        case manifest::Kind::data_slot: {
            native::SlotFn fn = native::find_slot(e.target);
            if (!fn) {
                fail(e, std::format("unknown native slot \"{}\"", e.target));
                break;
            }
            if (!*on) {
                ++stats.disabled;
                break;
            }
            operand_slots().push_back({e.addr, e.addr, mem::read<float>(e.addr), fn});
            ++stats.slotted;
            break;
        }
        case manifest::Kind::data_poke:
            if (!*on) {
                ++stats.disabled;
                break;
            }
            std::memcpy(mem::ptr(e.addr), e.bytes.data(), e.bytes.size());
            ++stats.poked;
            break;
        case manifest::Kind::byte_patch:
        case manifest::Kind::nop: {
            std::vector<uint8_t> bytes = e.kind == manifest::Kind::nop ? std::vector<uint8_t>(e.len, 0x90) : e.bytes;
            std::memcpy(mem::ptr(e.addr), bytes.data(), bytes.size());
            ++stats.lift_time;
            break;
        }
        case manifest::Kind::marker:
            ++stats.lift_time;
            break;
        }
    }
    if (!errors.empty()) {
        return std::unexpected(errors);
    }
    refresh_operand_slots();
    return stats;
}

void refresh_operand_slots() {
    for (const auto& s : operand_slots()) {
        mem::write<float>(s.slot, s.fn ? s.fn(s.initial) : mem::read<float>(s.original));
    }
}

}
