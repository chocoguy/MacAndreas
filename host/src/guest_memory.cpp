#include "guest_memory.hpp"

#include <sys/mman.h>

#include <bit>
#include <cstdio>
#include <format>

extern "C" {
__attribute__((visibility("default"))) uint8_t* ma_guest_base = nullptr;
}

namespace ma::host {

namespace mem {

std::string read_cstr(uint32_t addr, size_t max) {
    std::string out;
    for (size_t i = 0; i < max; ++i) {
        char c = static_cast<char>(*ptr(addr + static_cast<uint32_t>(i)));
        if (c == 0) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::u16string read_wstr(uint32_t addr, size_t max) {
    std::u16string out;
    for (size_t i = 0; i < max; ++i) {
        char16_t c = read<char16_t>(addr + static_cast<uint32_t>(i * 2));
        if (c == 0) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

void write_cstr(uint32_t addr, std::string_view s) {
    std::memcpy(ptr(addr), s.data(), s.size());
    *ptr(addr + static_cast<uint32_t>(s.size())) = 0;
}

void write_wstr(uint32_t addr, std::u16string_view s) {
    std::memcpy(ptr(addr), s.data(), s.size() * 2);
    write<char16_t>(addr + static_cast<uint32_t>(s.size() * 2), 0);
}

uint32_t host_to_guest(const void* p) {
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - ma_guest_base);
}

}

bool reserve_guest_space() {
    constexpr uint64_t kAlign = 1ull << 32;
    void* raw = mmap(nullptr, kGuestSpan + kAlign, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        return false;
    }
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + kAlign - 1) & ~(kAlign - 1);
    if (mprotect(reinterpret_cast<void*>(aligned + kNullGuardEnd), kGuestSpan - kNullGuardEnd - 0x10000,
                 PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    ma_guest_base = reinterpret_cast<uint8_t*>(aligned);
    return true;
}

std::expected<void, std::string> map_image(const pe::Image& image, const std::vector<uint8_t>& bytes) {
    std::memcpy(mem::ptr(image.image_base), bytes.data(), std::min<size_t>(image.size_of_headers, bytes.size()));
    for (const auto& s : image.sections) {
        uint32_t va = image.image_base + s.virtual_address;
        uint32_t n = std::min(s.raw_size, s.mapped_size());
        if (static_cast<uint64_t>(va) + s.mapped_size() > kAllocStart) {
            return std::unexpected(std::format("section {} overlaps the allocation area", s.name));
        }
        std::memcpy(mem::ptr(va), bytes.data() + s.raw_offset, n);
    }
    return {};
}

int GuestAllocator::size_class(uint32_t size) {
    uint32_t total = size + sizeof(Chunk);
    int cls = std::bit_width(total - 1) - 4;
    return cls < 0 ? 0 : cls;
}

uint32_t GuestAllocator::reserve(uint32_t size, uint32_t align) {
    std::lock_guard lock(mutex_);
    uint32_t start = (cursor_ + align - 1) & ~(align - 1);
    uint64_t end = static_cast<uint64_t>(start) + ((size + 0xfff) & ~0xfffu);
    if (end > kAllocEnd) {
        return 0;
    }
    cursor_ = static_cast<uint32_t>(end);
    std::memset(mem::ptr(start), 0, static_cast<size_t>(end - start));
    return start;
}

uint32_t GuestAllocator::alloc(uint32_t size) {
    std::lock_guard lock(mutex_);
    int cls = size_class(size);
    uint32_t block = 16u << cls;
    if (cls < kClasses && free_lists_[cls] != 0) {
        uint32_t chunk = free_lists_[cls];
        free_lists_[cls] = mem::read<uint32_t>(chunk + sizeof(Chunk));
        mem::write(chunk, Chunk{size, kMagic});
        std::memset(mem::ptr(chunk + sizeof(Chunk)), 0, size);
        return chunk + sizeof(Chunk);
    }
    if (cls >= kClasses) {
        uint32_t chunk = reserve(size + sizeof(Chunk), 0x1000);
        if (chunk == 0) {
            return 0;
        }
        mem::write(chunk, Chunk{size, kMagic});
        return chunk + sizeof(Chunk);
    }
    if (heap_cursor_ + block > heap_end_) {
        uint32_t grow = std::max<uint32_t>(block, 16u << 20);
        uint32_t region = reserve(grow, 0x10000);
        if (region == 0) {
            return 0;
        }
        heap_cursor_ = region;
        heap_end_ = region + grow;
    }
    uint32_t chunk = heap_cursor_;
    heap_cursor_ += block;
    mem::write(chunk, Chunk{size, kMagic});
    return chunk + sizeof(Chunk);
}

void GuestAllocator::free(uint32_t addr) {
    if (addr == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    uint32_t chunk = addr - sizeof(Chunk);
    auto header = mem::read<Chunk>(chunk);
    if (header.magic != kMagic) {
        std::fprintf(stderr, "[HEAP] free of unknown block 0x%08x\n", addr);
        return;
    }
    int cls = size_class(header.size);
    mem::write(chunk, Chunk{0, 0});
    if (cls >= kClasses) {
        return;
    }
    mem::write<uint32_t>(chunk + sizeof(Chunk), free_lists_[cls]);
    free_lists_[cls] = chunk;
}

uint32_t GuestAllocator::size_of(uint32_t addr) const {
    std::lock_guard lock(mutex_);
    auto header = mem::read<Chunk>(addr - sizeof(Chunk));
    return header.magic == kMagic ? header.size : 0xffffffffu;
}

bool GuestAllocator::owns(uint32_t addr) const {
    return addr >= kAllocStart + sizeof(Chunk) && addr < kAllocEnd && size_of(addr) != 0xffffffffu;
}

uint32_t GuestAllocator::realloc(uint32_t addr, uint32_t size) {
    if (addr == 0) {
        return alloc(size);
    }
    uint32_t old = size_of(addr);
    if (old == 0xffffffffu) {
        return 0;
    }
    if (size <= old && size_class(size) == size_class(old)) {
        mem::write(addr - sizeof(Chunk), Chunk{size, kMagic});
        return addr;
    }
    uint32_t fresh = alloc(size);
    if (fresh == 0) {
        return 0;
    }
    std::memcpy(mem::ptr(fresh), mem::ptr(addr), std::min(old, size));
    free(addr);
    return fresh;
}

GuestAllocator& allocator() {
    static GuestAllocator a;
    return a;
}

}
