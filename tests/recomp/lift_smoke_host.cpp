#include "check.hpp"

#include "macandreas/exe_identity.hpp"
#include "macandreas/pe_image.hpp"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

struct Memory;
struct State;

namespace {

constexpr uint64_t kGuestSpan = 1ull << 32;
constexpr uint32_t kStackTop = 0x00300000;
constexpr uint32_t kScratch = 0x00380000;

uint8_t* g_base = nullptr;
uint32_t g_esp = 0;
int g_unexpected_exits = 0;

template <typename T>
T load(uint32_t addr) {
    T v;
    std::memcpy(&v, g_base + addr, sizeof(T));
    return v;
}

template <typename T>
void store(uint32_t addr, T v) {
    std::memcpy(g_base + addr, &v, sizeof(T));
}

bool map_image(const ma::pe::Image& img, const std::vector<uint8_t>& bytes) {
    for (const auto& s : img.sections) {
        uint32_t va = img.image_base + s.virtual_address;
        if (static_cast<uint64_t>(va) + s.mapped_size() > kGuestSpan) {
            return false;
        }
        uint32_t n = s.raw_size < s.mapped_size() ? s.raw_size : s.mapped_size();
        std::memcpy(g_base + va, bytes.data() + s.raw_offset, n);
    }
    return true;
}

void push_call(std::initializer_list<uint32_t> args) {
    g_esp = kStackTop - 4 * static_cast<uint32_t>(args.size() + 1);
    store<uint32_t>(g_esp, 0xdeadbeef);
    uint32_t slot = g_esp + 4;
    for (uint32_t a : args) {
        store<uint32_t>(slot, a);
        slot += 4;
    }
}

using LiftedFn = uint32_t (*)();

void test_struct_field_subtract(LiftedFn fn) {
    store<uint32_t>(kScratch + 4, 100);
    store<uint32_t>(kScratch + 8, 0x11111111);
    push_call({kScratch, 30});
    uint32_t eax = fn();
    CHECK(eax == kScratch);
    CHECK(load<uint32_t>(kScratch + 4) == 70);
    CHECK(load<uint32_t>(kScratch + 8) == 0x11111111);

    push_call({kScratch, 100});
    fn();
    CHECK(load<uint32_t>(kScratch + 4) == 0xffffffe2u);
}

void test_global_table_lookup(LiftedFn fn) {
    constexpr uint32_t kFlags = 0x00ba82e0;
    constexpr uint32_t kPointers = 0x00ba82d8;
    constexpr uint32_t kObject = kScratch + 0x1000;

    store<uint8_t>(kFlags + 2, 0);
    push_call({0xffffff02});
    CHECK(fn() == 0x9d);

    store<uint8_t>(kFlags + 3, 1);
    store<uint32_t>(kPointers + 3 * 4, kObject);
    store<uint8_t>(kObject + 0x416, 0x5a);
    push_call({3});
    CHECK(fn() == ((kObject & 0xffffff00u) | 0x5a));
}

}

extern "C" {

uint32_t __remill_symbolic_STACK() { return g_esp; }
uint32_t __remill_symbolic_FSBASE() { return 0; }
uint32_t __remill_symbolic_GSBASE() { return 0; }

uint8_t __remill_read_memory_8(Memory*, uint32_t a) { return load<uint8_t>(a); }
uint16_t __remill_read_memory_16(Memory*, uint32_t a) { return load<uint16_t>(a); }
uint32_t __remill_read_memory_32(Memory*, uint32_t a) { return load<uint32_t>(a); }
uint64_t __remill_read_memory_64(Memory*, uint32_t a) { return load<uint64_t>(a); }

Memory* __remill_write_memory_8(Memory* m, uint32_t a, uint32_t v) {
    store<uint8_t>(a, static_cast<uint8_t>(v));
    return m;
}
Memory* __remill_write_memory_16(Memory* m, uint32_t a, uint32_t v) {
    store<uint16_t>(a, static_cast<uint16_t>(v));
    return m;
}
Memory* __remill_write_memory_32(Memory* m, uint32_t a, uint32_t v) {
    store<uint32_t>(a, v);
    return m;
}
Memory* __remill_write_memory_64(Memory* m, uint32_t a, uint64_t v) {
    store<uint64_t>(a, v);
    return m;
}

uint8_t __remill_undefined_8() { return 0; }
uint16_t __remill_undefined_16() { return 0; }
uint32_t __remill_undefined_32() { return 0; }
uint64_t __remill_undefined_64() { return 0; }

bool __remill_flag_computation_zero(bool result, ...) { return result; }
bool __remill_flag_computation_sign(bool result, ...) { return result; }
bool __remill_flag_computation_overflow(bool result, ...) { return result; }
bool __remill_flag_computation_carry(bool result, ...) { return result; }

bool __remill_compare_sle(bool result) { return result; }
bool __remill_compare_slt(bool result) { return result; }
bool __remill_compare_sge(bool result) { return result; }
bool __remill_compare_sgt(bool result) { return result; }
bool __remill_compare_ule(bool result) { return result; }
bool __remill_compare_ult(bool result) { return result; }
bool __remill_compare_ugt(bool result) { return result; }
bool __remill_compare_uge(bool result) { return result; }
bool __remill_compare_eq(bool result) { return result; }
bool __remill_compare_neq(bool result) { return result; }

Memory* __remill_function_return(State*, uint32_t, Memory* m) { return m; }

Memory* __remill_function_call(State*, uint32_t, Memory* m) {
    ++g_unexpected_exits;
    return m;
}
Memory* __remill_jump(State*, uint32_t, Memory* m) {
    ++g_unexpected_exits;
    return m;
}
Memory* __remill_missing_block(State*, uint32_t, Memory* m) {
    ++g_unexpected_exits;
    return m;
}
Memory* __remill_error(State*, uint32_t, Memory* m) {
    ++g_unexpected_exits;
    return m;
}
Memory* __remill_async_hyper_call(State*, uint32_t, Memory* m) {
    ++g_unexpected_exits;
    return m;
}
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: lift_smoke_host <gta_sa.exe> <lifted.dylib>\n");
        return 1;
    }
    auto bytes = ma::pe::read_file(argv[1]);
    if (!bytes || ma::exe::identify(*bytes).support != ma::exe::Support::supported) {
        std::fprintf(stderr, "lift_smoke_host: %s is not gta_sa.exe 1.0 US\n", argv[1]);
        return 1;
    }
    auto img = ma::pe::parse(*bytes);
    if (!img) {
        std::fprintf(stderr, "lift_smoke_host: %s\n", img.error().c_str());
        return 1;
    }

    void* region = mmap(nullptr, kGuestSpan, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        std::perror("mmap guest region");
        return 1;
    }
    g_base = static_cast<uint8_t*>(region);
    CHECK(map_image(*img, *bytes));

    void* lib = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "lift_smoke_host: %s\n", dlerror());
        return 1;
    }
    auto subtract = reinterpret_cast<LiftedFn>(dlsym(lib, "call_sub_809400"));
    auto lookup = reinterpret_cast<LiftedFn>(dlsym(lib, "call_sub_5807c0"));
    CHECK(subtract != nullptr);
    CHECK(lookup != nullptr);
    if (subtract) {
        test_struct_field_subtract(subtract);
    }
    if (lookup) {
        test_global_table_lookup(lookup);
    }
    CHECK(g_unexpected_exits == 0);
    return ma::test::finish("lift_smoke_host");
}
