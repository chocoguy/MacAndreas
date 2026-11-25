#pragma once

#include "macandreas/pe_image.hpp"

#include <cstdint>
#include <cstring>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

extern "C" uint8_t* ma_guest_base;

namespace ma::host {

inline constexpr uint64_t kGuestSpan = 1ull << 32;
inline constexpr uint32_t kNullGuardEnd = 0x00010000;
inline constexpr uint32_t kMainStackBottom = 0x00100000;
inline constexpr uint32_t kMainStackTop = 0x00300000;
inline constexpr uint32_t kAllocStart = 0x02000000;
inline constexpr uint32_t kAllocEnd = 0x70000000;
inline constexpr uint32_t kSystemArea = 0x7ff00000;
inline constexpr uint32_t kThunkBase = 0xf0000000;
inline constexpr uint32_t kThunkEnd = 0xf1000000;
inline constexpr uint32_t kReturnSentinel = 0xfffffff0;

namespace mem {

inline uint8_t* ptr(uint32_t addr) { return ma_guest_base + addr; }

template <typename T>
inline T read(uint32_t addr) {
    T v;
    std::memcpy(&v, ptr(addr), sizeof(T));
    return v;
}

template <typename T>
inline void write(uint32_t addr, T v) {
    std::memcpy(ptr(addr), &v, sizeof(T));
}

std::string read_cstr(uint32_t addr, size_t max = 65536);
std::u16string read_wstr(uint32_t addr, size_t max = 65536);
void write_cstr(uint32_t addr, std::string_view s);
void write_wstr(uint32_t addr, std::u16string_view s);
uint32_t host_to_guest(const void* p);

}

bool reserve_guest_space();
std::expected<void, std::string> map_image(const pe::Image& image, const std::vector<uint8_t>& bytes);

class GuestAllocator {
public:
    uint32_t reserve(uint32_t size, uint32_t align = 0x10000);
    uint32_t alloc(uint32_t size);
    void free(uint32_t addr);
    uint32_t size_of(uint32_t addr) const;
    uint32_t realloc(uint32_t addr, uint32_t size);
    bool owns(uint32_t addr) const;

private:
    struct Chunk {
        uint32_t size;
        uint32_t magic;
    };
    static constexpr uint32_t kMagic = 0x4d41484cu;
    static constexpr int kClasses = 24;
    static int size_class(uint32_t size);

    mutable std::recursive_mutex mutex_;
    uint32_t cursor_ = kAllocStart;
    uint32_t heap_cursor_ = 0;
    uint32_t heap_end_ = 0;
    uint32_t free_lists_[kClasses] = {};
};

GuestAllocator& allocator();

}
