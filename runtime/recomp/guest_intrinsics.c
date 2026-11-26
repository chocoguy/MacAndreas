#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct Memory Memory;

extern uint8_t *ma_guest_base;

#define MA_INLINE __attribute__((always_inline, visibility("hidden")))
#define GUEST(a) (ma_guest_base + (uint32_t)(a))

enum {
    kX87Invalid = 1 << 0,
    kX87Denormal = 1 << 1,
    kX87DivByZero = 1 << 2,
    kX87Overflow = 1 << 3,
    kX87Underflow = 1 << 4,
    kX87Precision = 1 << 5,
};

#define DEFINE_ACCESS(bits, type)                                                   \
    MA_INLINE type __remill_read_memory_##bits(Memory *m, uint32_t a) {             \
        type v;                                                                     \
        memcpy(&v, GUEST(a), sizeof v);                                             \
        return v;                                                                   \
    }                                                                               \
    MA_INLINE Memory *__remill_write_memory_##bits(Memory *m, uint32_t a, type v) { \
        memcpy(GUEST(a), &v, sizeof v);                                             \
        return m;                                                                   \
    }

DEFINE_ACCESS(8, uint8_t)
DEFINE_ACCESS(16, uint16_t)
DEFINE_ACCESS(32, uint32_t)
DEFINE_ACCESS(64, uint64_t)
DEFINE_ACCESS(f32, float)
DEFINE_ACCESS(f64, double)

MA_INLINE double ma_f80_to_double(const uint8_t *p) {
    uint64_t mantissa;
    uint16_t se;
    memcpy(&mantissa, p, 8);
    memcpy(&se, p + 8, 2);
    int negative = se >> 15;
    int exponent = se & 0x7fff;
    double v;
    if (exponent == 0 && mantissa == 0) {
        v = 0.0;
    } else if (exponent == 0x7fff) {
        v = (mantissa << 1) == 0 ? INFINITY : NAN;
    } else {
        v = ldexp((double)mantissa, exponent - 16383 - 63);
    }
    return negative ? -v : v;
}

MA_INLINE void ma_double_to_f80(double d, uint8_t *p) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint16_t sign = (uint16_t)((bits >> 63) << 15);
    uint32_t exponent = (uint32_t)((bits >> 52) & 0x7ff);
    uint64_t fraction = bits & 0xfffffffffffffull;
    uint64_t mantissa;
    uint16_t e80;
    if (exponent == 0 && fraction == 0) {
        mantissa = 0;
        e80 = 0;
    } else if (exponent == 0x7ff) {
        e80 = 0x7fff;
        mantissa = fraction == 0 ? 0x8000000000000000ull : (0xc000000000000000ull | (fraction << 11));
    } else if (exponent == 0) {
        int top = 63 - __builtin_clzll(fraction);
        mantissa = fraction << (63 - top);
        e80 = (uint16_t)(top - 1074 + 16383);
    } else {
        mantissa = 0x8000000000000000ull | (fraction << 11);
        e80 = (uint16_t)(exponent - 1023 + 16383);
    }
    uint16_t se = sign | e80;
    memcpy(p, &mantissa, 8);
    memcpy(p + 8, &se, 2);
}

MA_INLINE Memory *__remill_read_memory_f80(Memory *m, uint32_t a, uint8_t *out) {
    double v = ma_f80_to_double(GUEST(a));
    memcpy(out, &v, sizeof v);
    memcpy(out + sizeof v, GUEST(a) + 8, 2);
    return m;
}

MA_INLINE Memory *__remill_write_memory_f80(Memory *m, uint32_t a, const uint8_t *in) {
    double v;
    memcpy(&v, in, sizeof v);
    ma_double_to_f80(v, GUEST(a));
    return m;
}

MA_INLINE double __remill_read_memory_f128(Memory *m, uint32_t a) {
    return ma_f80_to_double(GUEST(a));
}

MA_INLINE Memory *__remill_write_memory_f128(Memory *m, uint32_t a, double v) {
    ma_double_to_f80(v, GUEST(a));
    memset(GUEST(a + 10), 0, 6);
    return m;
}

MA_INLINE uint8_t __remill_undefined_8(void) { return 0; }
MA_INLINE uint16_t __remill_undefined_16(void) { return 0; }
MA_INLINE uint32_t __remill_undefined_32(void) { return 0; }
MA_INLINE uint64_t __remill_undefined_64(void) { return 0; }
MA_INLINE float __remill_undefined_f32(void) { return 0.0f; }
MA_INLINE double __remill_undefined_f64(void) { return 0.0; }
MA_INLINE void __remill_undefined_f80(uint8_t *out) { memset(out, 0, 10); }
MA_INLINE double __remill_undefined_f128(void) { return 0.0; }

MA_INLINE bool __remill_flag_computation_zero(bool r, ...) { return r; }
MA_INLINE bool __remill_flag_computation_sign(bool r, ...) { return r; }
MA_INLINE bool __remill_flag_computation_overflow(bool r, ...) { return r; }
MA_INLINE bool __remill_flag_computation_carry(bool r, ...) { return r; }

MA_INLINE bool __remill_compare_sle(bool r) { return r; }
MA_INLINE bool __remill_compare_slt(bool r) { return r; }
MA_INLINE bool __remill_compare_sge(bool r) { return r; }
MA_INLINE bool __remill_compare_sgt(bool r) { return r; }
MA_INLINE bool __remill_compare_ule(bool r) { return r; }
MA_INLINE bool __remill_compare_ult(bool r) { return r; }
MA_INLINE bool __remill_compare_ugt(bool r) { return r; }
MA_INLINE bool __remill_compare_uge(bool r) { return r; }
MA_INLINE bool __remill_compare_eq(bool r) { return r; }
MA_INLINE bool __remill_compare_neq(bool r) { return r; }

MA_INLINE Memory *__remill_function_return(void *state, uint32_t pc, Memory *m) { return m; }

MA_INLINE Memory *__remill_barrier_load_load(Memory *m) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return m;
}
MA_INLINE Memory *__remill_barrier_load_store(Memory *m) {
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return m;
}
MA_INLINE Memory *__remill_barrier_store_load(Memory *m) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return m;
}
MA_INLINE Memory *__remill_barrier_store_store(Memory *m) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return m;
}
MA_INLINE Memory *__remill_atomic_begin(Memory *m) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return m;
}
MA_INLINE Memory *__remill_atomic_end(Memory *m) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return m;
}
MA_INLINE Memory *__remill_delay_slot_begin(Memory *m) { return m; }
MA_INLINE Memory *__remill_delay_slot_end(Memory *m) { return m; }

#define DEFINE_ATOMICS(bits, type)                                                                        \
    MA_INLINE Memory *__remill_compare_exchange_memory_##bits(Memory *m, uint32_t a, type *expected,     \
                                                              type desired) {                             \
        __atomic_compare_exchange_n((type *)GUEST(a), expected, desired, false, __ATOMIC_SEQ_CST,         \
                                    __ATOMIC_SEQ_CST);                                                    \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_add_##bits(Memory *m, uint32_t a, type *v) {                     \
        *v = __atomic_fetch_add((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                  \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_sub_##bits(Memory *m, uint32_t a, type *v) {                     \
        *v = __atomic_fetch_sub((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                  \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_and_##bits(Memory *m, uint32_t a, type *v) {                     \
        *v = __atomic_fetch_and((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                  \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_or_##bits(Memory *m, uint32_t a, type *v) {                      \
        *v = __atomic_fetch_or((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                   \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_xor_##bits(Memory *m, uint32_t a, type *v) {                     \
        *v = __atomic_fetch_xor((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                  \
        return m;                                                                                         \
    }                                                                                                     \
    MA_INLINE Memory *__remill_fetch_and_nand_##bits(Memory *m, uint32_t a, type *v) {                    \
        *v = __atomic_fetch_nand((type *)GUEST(a), *v, __ATOMIC_SEQ_CST);                                 \
        return m;                                                                                         \
    }

DEFINE_ATOMICS(8, uint8_t)
DEFINE_ATOMICS(16, uint16_t)
DEFINE_ATOMICS(32, uint32_t)
DEFINE_ATOMICS(64, uint64_t)

MA_INLINE int ma_host_excepts(int32_t x87) {
    int host = 0;
    if (x87 & kX87Invalid) host |= FE_INVALID;
    if (x87 & kX87DivByZero) host |= FE_DIVBYZERO;
    if (x87 & kX87Overflow) host |= FE_OVERFLOW;
    if (x87 & kX87Underflow) host |= FE_UNDERFLOW;
    if (x87 & kX87Precision) host |= FE_INEXACT;
    return host;
}

MA_INLINE int32_t ma_x87_excepts(int host) {
    int32_t x87 = 0;
    if (host & FE_INVALID) x87 |= kX87Invalid;
    if (host & FE_DIVBYZERO) x87 |= kX87DivByZero;
    if (host & FE_OVERFLOW) x87 |= kX87Overflow;
    if (host & FE_UNDERFLOW) x87 |= kX87Underflow;
    if (host & FE_INEXACT) x87 |= kX87Precision;
    return x87;
}

MA_INLINE int32_t __remill_fpu_exception_test(int32_t mask) {
    return ma_x87_excepts(fetestexcept(ma_host_excepts(mask)));
}

MA_INLINE void __remill_fpu_exception_clear(int32_t mask) { feclearexcept(ma_host_excepts(mask)); }

MA_INLINE void __remill_fpu_exception_raise(int32_t mask) { feraiseexcept(ma_host_excepts(mask)); }

MA_INLINE void __remill_fpu_set_rounding(int32_t rc) {
    static const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    fesetround(modes[rc & 3]);
}

MA_INLINE int32_t __remill_fpu_get_rounding(void) {
    switch (fegetround()) {
        case FE_DOWNWARD:
            return 1;
        case FE_UPWARD:
            return 2;
        case FE_TOWARDZERO:
            return 3;
        default:
            return 0;
    }
}

MA_INLINE uint8_t __remill_read_io_port_8(Memory *m, uint32_t a) { return 0; }
MA_INLINE uint16_t __remill_read_io_port_16(Memory *m, uint32_t a) { return 0; }
MA_INLINE uint32_t __remill_read_io_port_32(Memory *m, uint32_t a) { return 0; }
MA_INLINE Memory *__remill_write_io_port_8(Memory *m, uint32_t a, uint8_t v) { return m; }
MA_INLINE Memory *__remill_write_io_port_16(Memory *m, uint32_t a, uint16_t v) { return m; }
MA_INLINE Memory *__remill_write_io_port_32(Memory *m, uint32_t a, uint32_t v) { return m; }

MA_INLINE Memory *__remill_x86_set_segment_es(Memory *m) { return m; }
MA_INLINE Memory *__remill_x86_set_segment_ss(Memory *m) { return m; }
MA_INLINE Memory *__remill_x86_set_segment_ds(Memory *m) { return m; }
MA_INLINE Memory *__remill_x86_set_segment_fs(Memory *m) { return m; }
MA_INLINE Memory *__remill_x86_set_segment_gs(Memory *m) { return m; }
