#include "com.hpp"

#include "../guest_memory.hpp"

#include <map>
#include <mutex>
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

}
