#pragma once

#include "dispatch.hpp"
#include "guest_cpu.hpp"
#include "guest_memory.hpp"
#include "thunks.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ma::host {

template <typename T>
concept GuestScalar = std::is_integral_v<T> || std::is_enum_v<T> || std::is_floating_point_v<T>;

template <GuestScalar T>
constexpr unsigned slot_count() {
    return sizeof(T) > 4 ? 2 : 1;
}

inline double x87_st0(const GuestState& s) {
    double v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(&s.st.elems[0]) + 6, sizeof(v));
    return v;
}

inline void x87_set_st0(GuestState& s, double v) {
    std::memcpy(reinterpret_cast<uint8_t*>(&s.st.elems[0]) + 6, &v, sizeof(v));
}

inline void x87_push(GuestState& s, double v) {
    for (int i = 7; i > 0; --i) {
        std::memcpy(&s.st.elems[i], &s.st.elems[i - 1], sizeof(s.st.elems[i]));
    }
    x87_set_st0(s, v);
    s.x87.fxsave32.swd.top = static_cast<uint16_t>((s.x87.fxsave32.swd.top + 7) % 8);
}

inline double x87_pop(GuestState& s) {
    double v = x87_st0(s);
    for (int i = 0; i < 7; ++i) {
        std::memcpy(&s.st.elems[i], &s.st.elems[i + 1], sizeof(s.st.elems[i]));
    }
    s.x87.fxsave32.swd.top = static_cast<uint16_t>((s.x87.fxsave32.swd.top + 9) % 8);
    return v;
}

namespace abi_detail {

template <GuestScalar T>
void append_slots(std::array<uint32_t, 32>& out, unsigned& n, T value) {
    if constexpr (std::is_same_v<T, float>) {
        out[n++] = std::bit_cast<uint32_t>(value);
    } else if constexpr (std::is_same_v<T, double>) {
        auto bits = std::bit_cast<uint64_t>(value);
        out[n++] = static_cast<uint32_t>(bits);
        out[n++] = static_cast<uint32_t>(bits >> 32);
    } else if constexpr (sizeof(T) > 4) {
        auto bits = static_cast<uint64_t>(value);
        out[n++] = static_cast<uint32_t>(bits);
        out[n++] = static_cast<uint32_t>(bits >> 32);
    } else {
        out[n++] = static_cast<uint32_t>(value);
    }
}

template <GuestScalar T>
T read_slots(uint32_t addr) {
    if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(mem::read<uint32_t>(addr));
    } else if constexpr (std::is_same_v<T, double>) {
        return std::bit_cast<double>(mem::read<uint64_t>(addr));
    } else if constexpr (sizeof(T) > 4) {
        return static_cast<T>(mem::read<uint64_t>(addr));
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(mem::read<uint32_t>(addr));
    } else {
        return static_cast<T>(mem::read<uint32_t>(addr));
    }
}

template <GuestScalar R>
R read_return(GuestState& s) {
    if constexpr (std::is_floating_point_v<R>) {
        return static_cast<R>(x87_pop(s));
    } else if constexpr (sizeof(R) > 4) {
        return static_cast<R>((static_cast<uint64_t>(edx(s)) << 32) | eax(s));
    } else {
        return static_cast<R>(eax(s));
    }
}

template <GuestScalar R>
void write_return(GuestState& s, R value) {
    if constexpr (std::is_floating_point_v<R>) {
        x87_push(s, static_cast<double>(value));
    } else if constexpr (sizeof(R) > 4) {
        auto bits = static_cast<uint64_t>(value);
        eax(s) = static_cast<uint32_t>(bits);
        edx(s) = static_cast<uint32_t>(bits >> 32);
    } else {
        eax(s) = static_cast<uint32_t>(value);
    }
}

template <typename... Args>
constexpr unsigned total_slots() {
    return (0u + ... + slot_count<Args>());
}

}

template <Conv C, typename R = void, GuestScalar... Args>
R guest_call(GuestState& s, uint32_t target, Args... args) {
    std::array<uint32_t, 32> slots{};
    unsigned n = 0;
    if constexpr (C == Conv::thiscall) {
        static_assert(sizeof...(Args) >= 1, "thiscall needs a this pointer");
        auto pack = std::make_tuple(args...);
        ecx(s) = static_cast<uint32_t>(std::get<0>(pack));
        std::apply([&](auto, auto... rest) { (abi_detail::append_slots(slots, n, rest), ...); }, pack);
    } else {
        (abi_detail::append_slots(slots, n, args), ...);
    }
    call_guest(s, target, std::span<const uint32_t>(slots.data(), n), C != Conv::cdecl_);
    if constexpr (!std::is_void_v<R>) {
        return abi_detail::read_return<R>(s);
    }
}

template <Conv C, auto Fn>
struct NativeExport;

template <Conv C, typename R, typename... Args, R (*Fn)(Args...)>
struct NativeExport<C, Fn> {
    static constexpr bool kThis = C == Conv::thiscall;

    static constexpr unsigned stack_slots() {
        if constexpr (kThis) {
            return abi_detail::total_slots<Args...>() - 1;
        } else {
            return abi_detail::total_slots<Args...>();
        }
    }

    static void invoke(ApiCall& c) {
        invoke_impl(c, std::index_sequence_for<Args...>{});
    }

private:
    template <size_t... I>
    static void invoke_impl(ApiCall& c, std::index_sequence<I...>) {
        constexpr std::array<unsigned, sizeof...(Args) + 1> offsets = [] {
            std::array<unsigned, sizeof...(Args) + 1> o{};
            unsigned slot = 0;
            unsigned i = 0;
            ((o[i++] = slot, slot += slot_count<Args>()), ...);
            o[i] = slot;
            return o;
        }();
        uint32_t base = esp(c.s) + 4;
        auto arg = [&]<size_t K>() {
            using T = std::tuple_element_t<K, std::tuple<Args...>>;
            if constexpr (kThis) {
                if constexpr (K == 0) {
                    return static_cast<T>(ecx(c.s));
                } else {
                    return abi_detail::read_slots<T>(base + 4 * (offsets[K] - 1));
                }
            } else {
                return abi_detail::read_slots<T>(base + 4 * offsets[K]);
            }
        };
        if constexpr (std::is_void_v<R>) {
            Fn(arg.template operator()<I>()...);
        } else {
            abi_detail::write_return<R>(c.s, Fn(arg.template operator()<I>()...));
        }
    }
};

template <Conv C, auto Fn>
constexpr ApiDef native_export(std::string_view dll, std::string_view name, uint16_t ordinal = 0) {
    return ApiDef{dll, name, ordinal, static_cast<uint8_t>(NativeExport<C, Fn>::stack_slots()), C,
                  &NativeExport<C, Fn>::invoke};
}

}
