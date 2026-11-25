#include "thread.hpp"

#include <atomic>
#include <bitset>
#include <mutex>

namespace ma::host {

namespace {

thread_local GuestThread* t_current = nullptr;
std::atomic<uint32_t> g_next_thread = 0;
std::atomic<uint32_t> g_next_tid = 0x100;
std::mutex g_tls_mutex;
std::bitset<kTlsSlots> g_tls_used;

}

void init_process_block(uint32_t image_base, uint32_t process_heap) {
    std::memset(mem::ptr(kPeb), 0, 0x1000);
    mem::write<uint32_t>(kPeb + 0x08, image_base);
    mem::write<uint32_t>(kPeb + 0x18, process_heap);
    mem::write<uint32_t>(kPeb + 0xa4, 5);
    mem::write<uint32_t>(kPeb + 0xa8, 1);
    mem::write<uint16_t>(kPeb + 0xac, 2600);
    mem::write<uint32_t>(kPeb + 0xb0, 2);
}

GuestThread* create_guest_thread(uint32_t stack_bottom, uint32_t stack_top) {
    auto* t = new GuestThread();
    uint32_t index = g_next_thread++;
    t->teb = kTebBase + index * kTebStride;
    t->id = g_next_tid.fetch_add(4);
    t->stack_bottom = stack_bottom;
    t->stack_top = stack_top;

    uint32_t teb = t->teb;
    std::memset(mem::ptr(teb), 0, kTebStride);
    mem::write<uint32_t>(teb + 0x00, 0xffffffffu);
    mem::write<uint32_t>(teb + 0x04, stack_top);
    mem::write<uint32_t>(teb + 0x08, stack_bottom);
    mem::write<uint32_t>(teb + 0x18, teb);
    mem::write<uint32_t>(teb + 0x20, 0x1000);
    mem::write<uint32_t>(teb + 0x24, t->id);
    mem::write<uint32_t>(teb + 0x2c, teb + 0x1000);
    mem::write<uint32_t>(teb + 0x30, kPeb);

    GuestState& s = t->state;
    s.addr.fs_base.dword = teb;
    s.seg.cs.flat = 0x1b;
    s.seg.ss.flat = 0x23;
    s.seg.ds.flat = 0x23;
    s.seg.es.flat = 0x23;
    s.seg.fs.flat = 0x3b;
    s.seg.gs.flat = 0;
    esp(s) = stack_top - 16;
    s.x87.fxsave32.cwd.flat = 0x027f;
    s.x87.fxsave32.mxcsr.flat = 0x1f80;
    return t;
}

GuestThread* current_thread() { return t_current; }

void bind_current_thread(GuestThread* thread) { t_current = thread; }

uint32_t tls_alloc() {
    std::lock_guard lock(g_tls_mutex);
    for (uint32_t i = 0; i < kTlsSlots; ++i) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = true;
            return i;
        }
    }
    return 0xffffffffu;
}

bool tls_free(uint32_t slot) {
    std::lock_guard lock(g_tls_mutex);
    if (slot >= kTlsSlots || !g_tls_used[slot]) {
        return false;
    }
    g_tls_used[slot] = false;
    return true;
}

}
