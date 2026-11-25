#include "process.hpp"
#include "seh.hpp"
#include "sync.hpp"

#include "../dispatch.hpp"
#include "../thread.hpp"

#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kWaitObject0 = 0x00000000;
constexpr uint32_t kWaitTimeout = 0x00000102;
constexpr uint32_t kWaitFailed = 0xffffffffu;
constexpr uint32_t kInfinite = 0xffffffffu;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorAlreadyExists = 183;
constexpr uint32_t kErrorNotOwner = 288;
constexpr uint32_t kErrorTooManyPosts = 298;
constexpr uint32_t kCreateSuspended = 0x00000004;
constexpr uint32_t kStillActive = 259;
constexpr uint32_t kPseudoThread = 0xfffffffeu;
constexpr uint32_t kDefaultStack = 0x00100000;
constexpr size_t kHostThreadStack = 64ull << 20;

std::mutex g_sync;
std::condition_variable g_changed;
std::map<std::string, uint32_t> g_named;

struct Event final : Waitable {
    bool manual = false;
    bool signaled = false;
    bool try_acquire(uint32_t) override {
        if (!signaled) {
            return false;
        }
        if (!manual) {
            signaled = false;
        }
        return true;
    }
};

struct Semaphore final : Waitable {
    int32_t count = 0;
    int32_t maximum = 0;
    bool try_acquire(uint32_t) override {
        if (count <= 0) {
            return false;
        }
        --count;
        return true;
    }
};

struct Mutex final : Waitable {
    uint32_t owner = 0;
    uint32_t recursion = 0;
    bool try_acquire(uint32_t tid) override {
        if (owner != 0 && owner != tid) {
            return false;
        }
        owner = tid;
        ++recursion;
        return true;
    }
};

struct ThreadObject final : Waitable {
    GuestThread* thread = nullptr;
    uint32_t start = 0;
    uint32_t param = 0;
    uint32_t suspend_count = 0;
    int32_t priority = 0;
    bool finished = false;
    uint32_t exit_code = kStillActive;
    bool try_acquire(uint32_t) override { return finished; }
};

uint32_t current_tid() {
    auto* t = current_thread();
    return t ? t->id : 0;
}

uint32_t insert_named(uint32_t name_addr, std::shared_ptr<Waitable> object) {
    std::string name = name_addr ? mem::read_cstr(name_addr) : "";
    std::lock_guard lock(g_sync);
    if (!name.empty()) {
        if (auto it = g_named.find(name); it != g_named.end() && handle_lookup(it->second)) {
            set_last_error(kErrorAlreadyExists);
            return it->second;
        }
    }
    uint32_t h = handle_insert(std::move(object));
    if (!name.empty()) {
        g_named[name] = h;
    }
    set_last_error(0);
    return h;
}

void* host_thread_main(void* arg) {
    auto object = *static_cast<std::shared_ptr<ThreadObject>*>(arg);
    delete static_cast<std::shared_ptr<ThreadObject>*>(arg);
    {
        std::unique_lock lock(g_sync);
        g_changed.wait(lock, [&] { return object->suspend_count == 0; });
    }
    bind_current_thread(object->thread);
    uint32_t code = 0;
    try {
        code = call_guest(object->thread->state, object->start, {object->param}, true);
    } catch (const GuestUnwind& unwind) {
        std::fprintf(stderr, "[SEH] unwind to 0x%08x escaped thread %u\n", unwind.eip, object->thread->id);
        exit_process(1);
    }
    {
        std::lock_guard lock(g_sync);
        object->finished = true;
        object->exit_code = code;
    }
    g_changed.notify_all();
    return nullptr;
}

std::shared_ptr<ThreadObject> thread_object(uint32_t handle) {
    if (handle == kPseudoThread) {
        return nullptr;
    }
    return handle_get<ThreadObject>(handle);
}

constexpr ApiDef kSync[] = {
    {"KERNEL32.dll", "CreateEventA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         auto e = std::make_shared<Event>();
         e->manual = c.arg(1) != 0;
         e->signaled = c.arg(2) != 0;
         c.ret(insert_named(c.arg(3), e));
     }},
    {"KERNEL32.dll", "SetEvent", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto e = handle_get<Event>(c.arg(0));
         if (!e) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         {
             std::lock_guard lock(g_sync);
             e->signaled = true;
         }
         g_changed.notify_all();
         c.ret(1);
     }},
    {"KERNEL32.dll", "ResetEvent", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto e = handle_get<Event>(c.arg(0));
         if (!e) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         std::lock_guard lock(g_sync);
         e->signaled = false;
         c.ret(1);
     }},
    {"KERNEL32.dll", "CreateMutexA", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         auto m = std::make_shared<Mutex>();
         if (c.arg(1) != 0) {
             m->owner = current_tid();
             m->recursion = 1;
         }
         c.ret(insert_named(c.arg(2), m));
     }},
    {"KERNEL32.dll", "ReleaseMutex", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto m = handle_get<Mutex>(c.arg(0));
         if (!m) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         {
             std::lock_guard lock(g_sync);
             if (m->owner != current_tid() || m->recursion == 0) {
                 set_last_error(kErrorNotOwner);
                 c.ret(0);
                 return;
             }
             if (--m->recursion == 0) {
                 m->owner = 0;
             }
         }
         g_changed.notify_all();
         c.ret(1);
     }},
    {"KERNEL32.dll", "CreateSemaphoreA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         auto sem = std::make_shared<Semaphore>();
         sem->count = static_cast<int32_t>(c.arg(1));
         sem->maximum = static_cast<int32_t>(c.arg(2));
         c.ret(insert_named(c.arg(3), sem));
     }},
    {"KERNEL32.dll", "ReleaseSemaphore", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         auto sem = handle_get<Semaphore>(c.arg(0));
         if (!sem) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         auto release = static_cast<int32_t>(c.arg(1));
         {
             std::lock_guard lock(g_sync);
             if (release <= 0 || sem->count + release > sem->maximum) {
                 set_last_error(kErrorTooManyPosts);
                 c.ret(0);
                 return;
             }
             if (c.arg(2) != 0) {
                 mem::write<int32_t>(c.arg(2), sem->count);
             }
             sem->count += release;
         }
         g_changed.notify_all();
         c.ret(1);
     }},
    {"KERNEL32.dll", "WaitForSingleObject", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(wait_for(c.arg(0), c.arg(1))); }},
    {"KERNEL32.dll", "WaitForSingleObjectEx", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(wait_for(c.arg(0), c.arg(1))); }},
    {"KERNEL32.dll", "CreateThread", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t stack_size = c.arg(1) ? (c.arg(1) + 0xffffu) & ~0xffffu : kDefaultStack;
         uint32_t bottom = allocator().reserve(stack_size);
         if (bottom == 0) {
             set_last_error(8);
             c.ret(0);
             return;
         }
         auto object = std::make_shared<ThreadObject>();
         object->thread = create_guest_thread(bottom, bottom + stack_size);
         object->start = c.arg(2);
         object->param = c.arg(3);
         object->suspend_count = (c.arg(4) & kCreateSuspended) ? 1 : 0;
         uint32_t handle = handle_insert(object);
         if (c.arg(5) != 0) {
             mem::write<uint32_t>(c.arg(5), object->thread->id);
         }
         std::fprintf(stderr, "[THREAD] CreateThread(start=0x%08x, param=0x%08x, %s) -> tid %u\n", object->start,
                      object->param, object->suspend_count ? "suspended" : "running", object->thread->id);
         pthread_attr_t attr;
         pthread_attr_init(&attr);
         pthread_attr_setstacksize(&attr, kHostThreadStack);
         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
         pthread_t host;
         auto* arg = new std::shared_ptr<ThreadObject>(object);
         int rc = pthread_create(&host, &attr, host_thread_main, arg);
         pthread_attr_destroy(&attr);
         if (rc != 0) {
             delete arg;
             handle_close(handle);
             set_last_error(8);
             c.ret(0);
             return;
         }
         c.ret(handle);
     }},
    {"KERNEL32.dll", "ResumeThread", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto t = thread_object(c.arg(0));
         if (!t) {
             c.ret(0xffffffffu);
             return;
         }
         uint32_t previous = 0;
         {
             std::lock_guard lock(g_sync);
             previous = t->suspend_count;
             if (t->suspend_count > 0) {
                 --t->suspend_count;
             }
         }
         g_changed.notify_all();
         c.ret(previous);
     }},
    {"KERNEL32.dll", "SuspendThread", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[THREAD] SuspendThread(0x%08x) is not supported for running threads\n", c.arg(0));
         c.ret(0xffffffffu);
     }},
    {"KERNEL32.dll", "TerminateThread", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::fprintf(stderr, "[THREAD] TerminateThread(0x%08x) is not supported\n", c.arg(0));
         c.ret(0);
     }},
    {"KERNEL32.dll", "GetThreadPriority", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto t = thread_object(c.arg(0));
         c.ret(t ? static_cast<uint32_t>(t->priority) : 0);
     }},
    {"KERNEL32.dll", "SetThreadPriority", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         if (auto t = thread_object(c.arg(0))) {
             t->priority = static_cast<int32_t>(c.arg(1));
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetExitCodeThread", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto t = thread_object(c.arg(0));
         if (!t) {
             c.ret(0);
             return;
         }
         std::lock_guard lock(g_sync);
         mem::write<uint32_t>(c.arg(1), t->exit_code);
         c.ret(1);
     }},
};

}

uint32_t wait_for(uint32_t handle, uint32_t timeout_ms) {
    auto object = handle_get<Waitable>(handle);
    if (!object) {
        set_last_error(kErrorInvalidHandle);
        return kWaitFailed;
    }
    uint32_t tid = current_tid();
    std::unique_lock lock(g_sync);
    if (timeout_ms == kInfinite) {
        g_changed.wait(lock, [&] { return object->try_acquire(tid); });
        return kWaitObject0;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (g_changed.wait_until(lock, deadline, [&] { return object->try_acquire(tid); })) {
        return kWaitObject0;
    }
    return kWaitTimeout;
}

bool signal_event(uint32_t handle) {
    auto e = handle_get<Event>(handle);
    if (!e) {
        return false;
    }
    {
        std::lock_guard lock(g_sync);
        e->signaled = true;
    }
    g_changed.notify_all();
    return true;
}

std::span<const ApiDef> sync_apis() { return kSync; }

}
