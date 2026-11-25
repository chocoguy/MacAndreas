#pragma once

#include "com.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ma::host::win32 {

struct ComObject {
    virtual ~ComObject() = default;
    uint32_t guest = 0;
    uint32_t refs = 1;
    uint32_t slot = 0;
    ComInterface* iface = nullptr;
};

class ComObjects {
public:
    uint32_t wrap(ComInterface& iface, std::unique_ptr<ComObject> object);
    ComObject* find(uint32_t guest);
    uint32_t add_ref(uint32_t guest);
    uint32_t release(uint32_t guest);
    void destroy(uint32_t guest);

    template <typename T>
    T* get(uint32_t guest) {
        return dynamic_cast<T*>(find(guest));
    }

private:
    std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<ComObject>> slots_;
    std::vector<uint32_t> free_;
};

ComObjects& com_objects();

template <typename T>
T* com_get(uint32_t guest) {
    return com_objects().get<T>(guest);
}

inline uint32_t com_obj_add_ref(uint32_t self) { return com_objects().add_ref(self); }
inline uint32_t com_obj_release(uint32_t self) { return com_objects().release(self); }

inline uint32_t com_out(uint32_t out, uint32_t value) {
    if (out != 0) {
        mem::write<uint32_t>(out, value);
    }
    return value;
}

}
