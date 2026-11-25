#pragma once

#include "process.hpp"

#include <cstdint>

namespace ma::host::win32 {

struct Waitable : KObject {
    virtual bool try_acquire(uint32_t tid) = 0;
};

uint32_t wait_for(uint32_t handle, uint32_t timeout_ms);
bool signal_event(uint32_t handle);

}
