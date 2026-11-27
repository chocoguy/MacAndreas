#include "camera_pin.hpp"

#include "abi.hpp"
#include "thread.hpp"

#include <charconv>
#include <cstdio>

namespace ma::host::camera {

namespace {

bool parse_vec(std::string_view text, float (&out)[3]) {
    for (int i = 0; i < 3; ++i) {
        auto comma = text.find(',');
        std::string_view part = text.substr(0, comma);
        auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out[i]);
        if (ec != std::errc{} || end != part.data() + part.size()) {
            return false;
        }
        if ((i < 2) == (comma == std::string_view::npos)) {
            return false;
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return true;
}

uint32_t scratch() {
    static const uint32_t addr = allocator().alloc(36);
    return addr;
}

void write_vec(uint32_t addr, const float (&v)[3]) {
    for (int i = 0; i < 3; ++i) {
        mem::write<float>(addr + 4 * i, v[i]);
    }
}

}

std::optional<Settings::CameraPin> parse_pin(std::string_view text) {
    Settings::CameraPin pin{};
    auto colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto frame = text.substr(0, colon);
    auto [end, ec] = std::from_chars(frame.data(), frame.data() + frame.size(), pin.frame);
    if (ec != std::errc{} || end != frame.data() + frame.size()) {
        return std::nullopt;
    }
    auto rest = text.substr(colon + 1);
    if (rest == "restore") {
        pin.restore = true;
        return pin;
    }
    auto second = rest.find(':');
    if (second == std::string_view::npos || !parse_vec(rest.substr(0, second), pin.position) ||
        !parse_vec(rest.substr(second + 1), pin.target)) {
        return std::nullopt;
    }
    return pin;
}

void apply_pins(uint64_t frame) {
    for (const auto& pin : settings().camera_pins) {
        if (pin.frame != frame) {
            continue;
        }
        GuestThread* thread = current_thread();
        GuestState* s = thread ? &thread->state : nullptr;
        if (!s) {
            std::fprintf(stderr, "[CAMERA] frame %llu: no guest thread to pin the camera from\n",
                         static_cast<unsigned long long>(frame));
            continue;
        }
        uint32_t saved_eax = eax(*s);
        uint32_t saved_ecx = ecx(*s);
        uint32_t saved_edx = edx(*s);
        if (pin.restore) {
            guest_call<Conv::thiscall>(*s, kRestore, kTheCamera);
            std::fprintf(stderr, "[CAMERA] frame %llu: restored\n", static_cast<unsigned long long>(frame));
        } else {
            uint32_t position = scratch();
            uint32_t up = position + 12;
            uint32_t target = position + 24;
            write_vec(position, pin.position);
            write_vec(up, {0.0f, 0.0f, 0.0f});
            write_vec(target, pin.target);
            guest_call<Conv::thiscall>(*s, kSetCamPositionForFixedMode, kTheCamera, position, up);
            guest_call<Conv::thiscall>(*s, kTakeControlNoEntity, kTheCamera, target, kJumpCut, kScriptControl);
            std::fprintf(stderr, "[CAMERA] frame %llu: fixed at %.2f,%.2f,%.2f looking at %.2f,%.2f,%.2f\n",
                         static_cast<unsigned long long>(frame), pin.position[0], pin.position[1], pin.position[2],
                         pin.target[0], pin.target[1], pin.target[2]);
        }
        eax(*s) = saved_eax;
        ecx(*s) = saved_ecx;
        edx(*s) = saved_edx;
    }
}

}
