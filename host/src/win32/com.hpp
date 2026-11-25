#pragma once

#include "../abi.hpp"
#include "../thunks.hpp"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ma::host::win32 {

inline constexpr uint32_t kSOk = 0;
inline constexpr uint32_t kSFalse = 1;
inline constexpr uint32_t kENoInterface = 0x80004002u;
inline constexpr uint32_t kENotImpl = 0x80004001u;
inline constexpr uint32_t kEFail = 0x80004005u;
inline constexpr uint32_t kEInvalidArg = 0x80070057u;

struct ComClass {
    std::string_view dll;
    std::span<const std::string_view> methods;
};

uint32_t com_create(const ComClass& cls, uint32_t extra_bytes = 0);
uint32_t com_add_ref(uint32_t object);
uint32_t com_release(uint32_t object);
uint32_t com_extra(uint32_t object);

struct ComMethod {
    std::string_view name;
    uint8_t args;
    ApiFn fn = nullptr;
    uint32_t fallback = kSOk;
};

template <auto Fn>
constexpr ComMethod com_method(std::string_view name) {
    return ComMethod{name, static_cast<uint8_t>(NativeExport<Conv::stdcall, Fn>::stack_slots()),
                     &NativeExport<Conv::stdcall, Fn>::invoke, kSOk};
}

class ComInterface {
public:
    ComInterface(std::string_view dll, std::string_view name, std::initializer_list<ComMethod> methods);

    uint32_t vtable();
    uint32_t create(uint32_t extra_bytes = 0);
    std::string_view name() const { return name_; }
    size_t size() const { return methods_.size(); }

private:
    std::string dll_;
    std::string name_;
    std::vector<ComMethod> methods_;
    std::deque<std::string> names_;
    std::vector<ApiDef> defs_;
    std::once_flag once_;
    uint32_t vtable_ = 0;
};

void com_unimplemented(ApiCall& c);
void com_noop(ApiCall& c);
std::string guid_string(uint32_t addr);
bool guid_equals(uint32_t addr, uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4);

}
