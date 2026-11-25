#include "com.hpp"

#include "../guest_memory.hpp"

#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kHeaderSize = 8;

std::mutex g_mutex;
std::map<const void*, uint32_t> g_vtables;

uint32_t vtable_for(const ComClass& cls) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_vtables.find(cls.methods.data()); it != g_vtables.end()) {
        return it->second;
    }
    uint32_t table = allocator().alloc(static_cast<uint32_t>(cls.methods.size() * 4));
    for (size_t i = 0; i < cls.methods.size(); ++i) {
        mem::write<uint32_t>(table + static_cast<uint32_t>(4 * i), thunk_for(cls.dll, cls.methods[i], 0));
    }
    g_vtables[cls.methods.data()] = table;
    return table;
}

std::mutex g_unimplemented_mutex;
std::set<std::string_view> g_unimplemented_seen;

}

uint32_t com_create(const ComClass& cls, uint32_t extra_bytes) {
    uint32_t object = allocator().alloc(kHeaderSize + extra_bytes);
    std::memset(mem::ptr(object), 0, kHeaderSize + extra_bytes);
    mem::write<uint32_t>(object, vtable_for(cls));
    mem::write<uint32_t>(object + 4, 1);
    return object;
}

uint32_t com_add_ref(uint32_t object) {
    uint32_t count = mem::read<uint32_t>(object + 4) + 1;
    mem::write<uint32_t>(object + 4, count);
    return count;
}

uint32_t com_release(uint32_t object) {
    uint32_t count = mem::read<uint32_t>(object + 4);
    if (count == 0) {
        return 0;
    }
    mem::write<uint32_t>(object + 4, --count);
    if (count == 0) {
        allocator().free(object);
    }
    return count;
}

uint32_t com_extra(uint32_t object) { return object + kHeaderSize; }

ComInterface::ComInterface(std::string_view dll, std::string_view name, std::initializer_list<ComMethod> methods)
    : dll_(dll), name_(name), methods_(methods) {}

uint32_t ComInterface::vtable() {
    std::call_once(once_, [this] {
        defs_.reserve(methods_.size());
        for (const auto& m : methods_) {
            const std::string& full = names_.emplace_back(std::format("{}::{}", name_, m.name));
            ApiDef def{dll_, full, 0, m.args, Conv::stdcall, m.fn ? m.fn : com_unimplemented};
            def.value = m.fallback;
            defs_.push_back(def);
        }
        register_apis(defs_);
        vtable_ = allocator().alloc(static_cast<uint32_t>(defs_.size() * 4));
        for (size_t i = 0; i < defs_.size(); ++i) {
            mem::write<uint32_t>(vtable_ + static_cast<uint32_t>(4 * i), thunk_for(dll_, defs_[i].name, 0));
        }
    });
    return vtable_;
}

uint32_t ComInterface::create(uint32_t extra_bytes) {
    uint32_t object = allocator().alloc(kHeaderSize + extra_bytes);
    std::memset(mem::ptr(object), 0, kHeaderSize + extra_bytes);
    mem::write<uint32_t>(object, vtable());
    mem::write<uint32_t>(object + 4, 1);
    return object;
}

void com_unimplemented(ApiCall& c) {
    const ApiDef* def = c.def;
    if (!def) {
        c.ret(kENotImpl);
        return;
    }
    {
        std::lock_guard lock(g_unimplemented_mutex);
        if (g_unimplemented_seen.insert(def->name).second) {
            std::fprintf(stderr, "[COM] %.*s!%.*s not implemented (caller 0x%08x), returning 0x%08x\n",
                         static_cast<int>(def->dll.size()), def->dll.data(), static_cast<int>(def->name.size()),
                         def->name.data(), mem::read<uint32_t>(esp(c.s)), def->value);
        }
    }
    c.ret(def->value);
}

void com_noop(ApiCall& c) { c.ret(c.def ? c.def->value : kSOk); }

std::string guid_string(uint32_t addr) {
    if (addr == 0) {
        return "null";
    }
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                       mem::read<uint32_t>(addr), mem::read<uint16_t>(addr + 4), mem::read<uint16_t>(addr + 6),
                       mem::read<uint8_t>(addr + 8), mem::read<uint8_t>(addr + 9), mem::read<uint8_t>(addr + 10),
                       mem::read<uint8_t>(addr + 11), mem::read<uint8_t>(addr + 12), mem::read<uint8_t>(addr + 13),
                       mem::read<uint8_t>(addr + 14), mem::read<uint8_t>(addr + 15));
}

bool guid_equals(uint32_t addr, uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4) {
    if (addr == 0 || mem::read<uint32_t>(addr) != d1 || mem::read<uint16_t>(addr + 4) != d2 ||
        mem::read<uint16_t>(addr + 6) != d3) {
        return false;
    }
    uint32_t i = 0;
    for (uint8_t b : d4) {
        if (mem::read<uint8_t>(addr + 8 + i++) != b) {
            return false;
        }
    }
    return true;
}

}

#include "com_object.hpp"

namespace ma::host::win32 {

namespace {

constexpr uint32_t kSlotMagic = 0x4d414f42u;

}

uint32_t ComObjects::wrap(ComInterface& iface, std::unique_ptr<ComObject> object) {
    std::lock_guard lock(mutex_);
    uint32_t slot;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else {
        slot = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    uint32_t guest = iface.create(8);
    object->guest = guest;
    object->refs = 1;
    object->slot = slot;
    object->iface = &iface;
    mem::write<uint32_t>(com_extra(guest), kSlotMagic);
    mem::write<uint32_t>(com_extra(guest) + 4, slot);
    slots_[slot] = std::move(object);
    return guest;
}

ComObject* ComObjects::find(uint32_t guest) {
    if (guest == 0 || !allocator().owns(guest)) {
        return nullptr;
    }
    if (mem::read<uint32_t>(com_extra(guest)) != kSlotMagic) {
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    uint32_t slot = mem::read<uint32_t>(com_extra(guest) + 4);
    if (slot >= slots_.size() || !slots_[slot] || slots_[slot]->guest != guest) {
        return nullptr;
    }
    return slots_[slot].get();
}

uint32_t ComObjects::add_ref(uint32_t guest) {
    std::lock_guard lock(mutex_);
    ComObject* o = find(guest);
    if (!o) {
        return 0;
    }
    mem::write<uint32_t>(guest + 4, ++o->refs);
    return o->refs;
}

uint32_t ComObjects::release(uint32_t guest) {
    std::unique_ptr<ComObject> dead;
    {
        std::lock_guard lock(mutex_);
        ComObject* o = find(guest);
        if (!o || o->refs == 0) {
            return 0;
        }
        mem::write<uint32_t>(guest + 4, --o->refs);
        if (o->refs != 0) {
            return o->refs;
        }
        dead = std::move(slots_[o->slot]);
        free_.push_back(dead->slot);
    }
    mem::write<uint32_t>(com_extra(guest), 0);
    dead.reset();
    allocator().free(guest);
    return 0;
}

void ComObjects::destroy(uint32_t guest) {
    std::unique_ptr<ComObject> dead;
    {
        std::lock_guard lock(mutex_);
        ComObject* o = find(guest);
        if (!o) {
            return;
        }
        dead = std::move(slots_[o->slot]);
        free_.push_back(dead->slot);
    }
    mem::write<uint32_t>(com_extra(guest), 0);
    dead.reset();
    allocator().free(guest);
}

ComObjects& com_objects() {
    static ComObjects objects;
    return objects;
}

}
